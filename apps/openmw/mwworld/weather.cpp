#define _USE_MATH_DEFINES
#include <cmath>

#include "weather.hpp"

#include <components/misc/rng.hpp>

#include <components/esm/esmreader.hpp>
#include <components/esm/esmwriter.hpp>
#include <components/esm/savedgame.hpp>
#include <components/esm/weatherstate.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwbase/soundmanager.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwsound/sound.hpp"

#include "../mwrender/renderingmanager.hpp"
#include "../mwrender/sky.hpp"

#include "player.hpp"
#include "esmstore.hpp"
#include "fallback.hpp"
#include "cellstore.hpp"

using namespace MWWorld;

namespace
{
    static const int invalidWeatherID = -1;

    float lerp (float x, float y, float factor)
    {
        return x * (1-factor) + y * factor;
    }

    osg::Vec4f lerp (const osg::Vec4f& x, const osg::Vec4f& y, float factor)
    {
        return x * (1-factor) + y * factor;
    }
}

Weather::Weather(const std::string& name,
                 const MWWorld::Fallback& fallback,
                 float stormWindSpeed,
                 float rainSpeed,
                 const std::string& ambientLoopSoundID,
                 const std::string& particleEffect)
    : mCloudTexture(fallback.getFallbackString("Weather_" + name + "_Cloud_Texture"))
    , mSkySunriseColor(fallback.getFallbackColour("Weather_" + name +"_Sky_Sunrise_Color"))
    , mSkyDayColor(fallback.getFallbackColour("Weather_" + name + "_Sky_Day_Color"))
    , mSkySunsetColor(fallback.getFallbackColour("Weather_" + name + "_Sky_Sunset_Color"))
    , mSkyNightColor(fallback.getFallbackColour("Weather_" + name + "_Sky_Night_Color"))
    , mFogSunriseColor(fallback.getFallbackColour("Weather_" + name + "_Fog_Sunrise_Color"))
    , mFogDayColor(fallback.getFallbackColour("Weather_" + name + "_Fog_Day_Color"))
    , mFogSunsetColor(fallback.getFallbackColour("Weather_" + name + "_Fog_Sunset_Color"))
    , mFogNightColor(fallback.getFallbackColour("Weather_" + name + "_Fog_Night_Color"))
    , mAmbientSunriseColor(fallback.getFallbackColour("Weather_" + name + "_Ambient_Sunrise_Color"))
    , mAmbientDayColor(fallback.getFallbackColour("Weather_" + name + "_Ambient_Day_Color"))
    , mAmbientSunsetColor(fallback.getFallbackColour("Weather_" + name + "_Ambient_Sunset_Color"))
    , mAmbientNightColor(fallback.getFallbackColour("Weather_" + name + "_Ambient_Night_Color"))
    , mSunSunriseColor(fallback.getFallbackColour("Weather_" + name + "_Sun_Sunrise_Color"))
    , mSunDayColor(fallback.getFallbackColour("Weather_" + name + "_Sun_Day_Color"))
    , mSunSunsetColor(fallback.getFallbackColour("Weather_" + name + "_Sun_Sunset_Color"))
    , mSunNightColor(fallback.getFallbackColour("Weather_" + name + "_Sun_Night_Color"))
    , mLandFogDayDepth(fallback.getFallbackFloat("Weather_" + name + "_Land_Fog_Day_Depth"))
    , mLandFogNightDepth(fallback.getFallbackFloat("Weather_" + name + "_Land_Fog_Night_Depth"))
    , mSunDiscSunsetColor(fallback.getFallbackColour("Weather_" + name + "_Sun_Disc_Sunset_Color"))
    , mWindSpeed(fallback.getFallbackFloat("Weather_" + name + "_Wind_Speed"))
    , mCloudSpeed(fallback.getFallbackFloat("Weather_" + name + "_Cloud_Speed"))
    , mGlareView(fallback.getFallbackFloat("Weather_" + name + "_Glare_View"))
    , mAmbientLoopSoundID(ambientLoopSoundID)
    , mIsStorm(mWindSpeed > stormWindSpeed)
    , mRainSpeed(rainSpeed)
    , mRainFrequency(fallback.getFallbackFloat("Weather_" + name + "_Rain_Entrance_Speed"))
    , mParticleEffect(particleEffect)
    , mRainEffect(fallback.getFallbackBool("Weather_" + name + "_Using_Precip") ? "meshes\\raindrop.nif" : "")
    , mTransitionDelta(fallback.getFallbackFloat("Weather_" + name + "_Transition_Delta"))
    , mCloudsMaximumPercent(fallback.getFallbackFloat("Weather_" + name + "_Clouds_Maximum_Percent"))
{
/*
Unhandled:
Rain Diameter=600 ?
Rain Height Min=200 ?
Rain Height Max=700 ?
Rain Threshold=0.6 ?
Max Raindrops=650 ?
*/
}

float Weather::transitionDelta() const
{
    // Transition Delta describes how quickly transitioning to the weather in question will take, in Hz. Note that the
    // measurement is in real time, not in-game time.
    return mTransitionDelta;
}

float Weather::cloudBlendFactor(float transitionRatio) const
{
    // Clouds Maximum Percent affects how quickly the sky transitions from one sky texture to the next.
    return transitionRatio / mCloudsMaximumPercent;
}

RegionWeather::RegionWeather(const ESM::Region& region)
    : mWeather(invalidWeatherID)
    , mChances()
{
    mChances.reserve(10);
    mChances.push_back(region.mData.mClear);
    mChances.push_back(region.mData.mCloudy);
    mChances.push_back(region.mData.mFoggy);
    mChances.push_back(region.mData.mOvercast);
    mChances.push_back(region.mData.mRain);
    mChances.push_back(region.mData.mThunder);
    mChances.push_back(region.mData.mAsh);
    mChances.push_back(region.mData.mBlight);
    mChances.push_back(region.mData.mA);
    mChances.push_back(region.mData.mB);
}

RegionWeather::RegionWeather(const ESM::RegionWeatherState& state)
    : mWeather(state.mWeather)
    , mChances(state.mChances)
{
}

RegionWeather::operator ESM::RegionWeatherState() const
{
    ESM::RegionWeatherState state =
        {
            mWeather,
            mChances
        };

    return state;
}

void RegionWeather::setChances(const std::vector<char>& chances)
{
    if(mChances.size() < chances.size())
    {
        mChances.reserve(chances.size());
    }

    std::vector<char>::const_iterator it = chances.begin();
    for(size_t i = 0; it != chances.end(); ++it, ++i)
    {
        mChances[i] = *it;
    }

    // Regional weather no longer supports the current type, select a new weather pattern.
    if((static_cast<size_t>(mWeather) >= mChances.size()) || (mChances[mWeather] == 0))
    {
        chooseNewWeather();
    }
}

void RegionWeather::setWeather(int weatherID)
{
    mWeather = weatherID;
}

int RegionWeather::getWeather()
{
    // If the region weather was already set (by ChangeWeather, or by a previous call) then just return that value.
    // Note that the region weather will be expired periodically when the weather update timer expires.
    if(mWeather == invalidWeatherID)
    {
        chooseNewWeather();
    }

    return mWeather;
}

void RegionWeather::chooseNewWeather()
{
    // All probabilities must add to 100 (responsibility of the user).
    // If chances A and B has values 30 and 70 then by generating 100 numbers 1..100, 30% will be lesser or equal 30
    // and 70% will be greater than 30 (in theory).
    int chance = Misc::Rng::rollDice(100) + 1; // 1..100
    int sum = 0;
    int i = 0;
    for(; static_cast<size_t>(i) < mChances.size(); ++i)
    {
        sum += mChances[i];
        if(chance <= sum)
            break;
    }

    mWeather = i;
}

MoonModel::MoonModel(const std::string& name, const MWWorld::Fallback& fallback)
  : mFadeInStart(fallback.getFallbackFloat("Moons_" + name + "_Fade_In_Start"))
  , mFadeInFinish(fallback.getFallbackFloat("Moons_" + name + "_Fade_In_Finish"))
  , mFadeOutStart(fallback.getFallbackFloat("Moons_" + name + "_Fade_Out_Start"))
  , mFadeOutFinish(fallback.getFallbackFloat("Moons_" + name + "_Fade_Out_Finish"))
  , mAxisOffset(fallback.getFallbackFloat("Moons_" + name + "_Axis_Offset"))
  , mSpeed(fallback.getFallbackFloat("Moons_" + name + "_Speed"))
  , mDailyIncrement(fallback.getFallbackFloat("Moons_" + name + "_Daily_Increment"))
  , mFadeStartAngle(fallback.getFallbackFloat("Moons_" + name + "_Fade_Start_Angle"))
  , mFadeEndAngle(fallback.getFallbackFloat("Moons_" + name + "_Fade_End_Angle"))
  , mMoonShadowEarlyFadeAngle(fallback.getFallbackFloat("Moons_" + name + "_Moon_Shadow_Early_Fade_Angle"))
{
    // Morrowind appears to have a minimum speed in order to avoid situations where the moon couldn't conceivably
    // complete a rotation in a single 24 hour period. The value of 180/23 was deduced from reverse engineering.
    mSpeed = std::min(mSpeed, 180.0f / 23.0f);
}

MWRender::MoonState MoonModel::calculateState(const TimeStamp& gameTime) const
{
    float rotationFromHorizon = angle(gameTime);
    MWRender::MoonState state =
        {
            rotationFromHorizon,
            mAxisOffset, // Reverse engineered from Morrowind's scene graph rotation matrices.
            static_cast<MWRender::MoonState::Phase>(phase(gameTime)),
            shadowBlend(rotationFromHorizon),
            earlyMoonShadowAlpha(rotationFromHorizon) * hourlyAlpha(gameTime.getHour())
        };

    return state;
}

inline float MoonModel::angle(const TimeStamp& gameTime) const
{
    // Morrowind's moons start travel on one side of the horizon (let's call it H-rise) and travel 180 degrees to the
    // opposite horizon (let's call it H-set). Upon reaching H-set, they reset to H-rise until the next moon rise.

    // When calculating the angle of the moon, several cases have to be taken into account:
    // 1. Moon rises and then sets in one day.
    // 2. Moon sets and doesn't rise in one day (occurs when the moon rise hour is >= 24).
    // 3. Moon sets and then rises in one day.
    float moonRiseHourToday = moonRiseHour(gameTime.getDay());
    float moonRiseAngleToday = 0;

    if(gameTime.getHour() < moonRiseHourToday)
    {
        float moonRiseHourYesterday = moonRiseHour(gameTime.getDay() - 1);
        if(moonRiseHourYesterday < 24)
        {
            float moonRiseAngleYesterday = rotation(24 - moonRiseHourYesterday);
            if(moonRiseAngleYesterday < 180)
            {
                // The moon rose but did not set yesterday, so accumulate yesterday's angle with how much we've travelled today.
                moonRiseAngleToday = rotation(gameTime.getHour()) + moonRiseAngleYesterday;
            }
        }
    }
    else
    {
        moonRiseAngleToday = rotation(gameTime.getHour() - moonRiseHourToday);
    }

    if(moonRiseAngleToday >= 180)
    {
        // The moon set today, reset the angle to the horizon.
        moonRiseAngleToday = 0;
    }

    return moonRiseAngleToday;
}

inline float MoonModel::moonRiseHour(unsigned int daysPassed) const
{
    // This arises from the start date of 16 Last Seed, 427
    // TODO: Find an alternate formula that doesn't rely on this day being fixed.
    static const unsigned int startDay = 16;

    // This odd formula arises from the fact that on 16 Last Seed, 17 increments have occurred, meaning
    // that upon starting a new game, it must only calculate the moon phase as far back as 1 Last Seed.
    // Note that we don't modulo after adding the latest daily increment because other calculations need to
    // know if doing so would cause the moon rise to be postponed until the next day (which happens when
    // the moon rise hour is >= 24 in Morrowind).
    return mDailyIncrement + std::fmod((daysPassed - 1 + startDay) * mDailyIncrement, 24.0f);
}

inline float MoonModel::rotation(float hours) const
{
    // 15 degrees per hour was reverse engineered from the rotation matrices of the Morrowind scene graph.
    // Note that this correlates to 360 / 24, which is a full rotation every 24 hours, so speed is a measure
    // of whole rotations that could be completed in a day.
    return 15.0f * mSpeed * hours;
}

inline unsigned int MoonModel::phase(const TimeStamp& gameTime) const
{
    // Morrowind starts with a full moon on 16 Last Seed and then begins to wane 17 Last Seed, working on 3 day phase cycle.
    // Note: this is an internal helper, and as such we don't want to return MWRender::MoonState::Phase since we can't
    // forward declare it (C++11 strongly typed enums solve this).

    // If the moon didn't rise yet today, use yesterday's moon phase.
    if(gameTime.getHour() < moonRiseHour(gameTime.getDay()))
        return (gameTime.getDay() / 3) % 8;
    else
        return ((gameTime.getDay() + 1) / 3) % 8;
}

inline float MoonModel::shadowBlend(float angle) const
{
    // The Fade End Angle and Fade Start Angle describe a region where the moon transitions from a solid disk
    // that is roughly the color of the sky, to a textured surface.
    // Depending on the current angle, the following values describe the ratio between the textured moon
    // and the solid disk:
    // 1. From Fade End Angle 1 to Fade Start Angle 1 (during moon rise): 0..1
    // 2. From Fade Start Angle 1 to Fade Start Angle 2 (between moon rise and moon set): 1 (textured)
    // 3. From Fade Start Angle 2 to Fade End Angle 2 (during moon set): 1..0
    // 4. From Fade End Angle 2 to Fade End Angle 1 (between moon set and moon rise): 0 (solid disk)
    float fadeAngle = mFadeStartAngle - mFadeEndAngle;
    float fadeEndAngle2 = 180.0f - mFadeEndAngle;
    float fadeStartAngle2 = 180.0f - mFadeStartAngle;
    if((angle >= mFadeEndAngle) && (angle < mFadeStartAngle))
        return (angle - mFadeEndAngle) / fadeAngle;
    else if((angle >= mFadeStartAngle) && (angle < fadeStartAngle2))
        return 1.0f;
    else if((angle >= fadeStartAngle2) && (angle < fadeEndAngle2))
        return (fadeEndAngle2 - angle) / fadeAngle;
    else
        return 0.0f;
}

inline float MoonModel::hourlyAlpha(float gameHour) const
{
    // The Fade Out Start / Finish and Fade In Start / Finish describe the hours at which the moon
    // appears and disappears.
    // Depending on the current hour, the following values describe how transparent the moon is.
    // 1. From Fade Out Start to Fade Out Finish: 1..0
    // 2. From Fade Out Finish to Fade In Start: 0 (transparent)
    // 3. From Fade In Start to Fade In Finish: 0..1
    // 4. From Fade In Finish to Fade Out Start: 1 (solid)
    if((gameHour >= mFadeOutStart) && (gameHour < mFadeOutFinish))
        return (mFadeOutFinish - gameHour) / (mFadeOutFinish - mFadeOutStart);
    else if((gameHour >= mFadeOutFinish) && (gameHour < mFadeInStart))
        return 0.0f;
    else if((gameHour >= mFadeInStart) && (gameHour < mFadeInFinish))
        return (gameHour - mFadeInStart) / (mFadeInFinish - mFadeInStart);
    else
        return 1.0f;
}

inline float MoonModel::earlyMoonShadowAlpha(float angle) const
{
    // The Moon Shadow Early Fade Angle describes an arc relative to Fade End Angle.
    // Depending on the current angle, the following values describe how transparent the moon is.
    // 1. From Moon Shadow Early Fade Angle 1 to Fade End Angle 1 (during moon rise): 0..1
    // 2. From Fade End Angle 1 to Fade End Angle 2 (between moon rise and moon set): 1 (solid)
    // 3. From Fade End Angle 2 to Moon Shadow Early Fade Angle 2 (during moon set): 1..0
    // 4. From Moon Shadow Early Fade Angle 2 to Moon Shadow Early Fade Angle 1: 0 (transparent)
    float moonShadowEarlyFadeAngle1 = mFadeEndAngle - mMoonShadowEarlyFadeAngle;
    float fadeEndAngle2 = 180.0f - mFadeEndAngle;
    float moonShadowEarlyFadeAngle2 = fadeEndAngle2 + mMoonShadowEarlyFadeAngle;
    if((angle >= moonShadowEarlyFadeAngle1) && (angle < mFadeEndAngle))
        return (angle - moonShadowEarlyFadeAngle1) / mMoonShadowEarlyFadeAngle;
    else if((angle >= mFadeEndAngle) && (angle < fadeEndAngle2))
        return 1.0f;
    else if((angle >= fadeEndAngle2) && (angle < moonShadowEarlyFadeAngle2))
        return (moonShadowEarlyFadeAngle2 - angle) / mMoonShadowEarlyFadeAngle;
    else
        return 0.0f;
}

WeatherManager::WeatherManager(MWRender::RenderingManager& rendering, const MWWorld::Fallback& fallback, MWWorld::ESMStore& store)
    : mStore(store)
    , mRendering(rendering)
    , mSunriseTime(fallback.getFallbackFloat("Weather_Sunrise_Time"))
    , mSunsetTime(fallback.getFallbackFloat("Weather_Sunset_Time"))
    , mSunriseDuration(fallback.getFallbackFloat("Weather_Sunrise_Duration"))
    , mSunsetDuration(fallback.getFallbackFloat("Weather_Sunset_Duration"))
    , mNightStart(mSunsetTime + mSunsetDuration)
    , mNightEnd(mSunriseTime - 0.5f)
    , mDayStart(mSunriseTime + mSunriseDuration)
    , mDayEnd(mSunsetTime)
    , mHoursBetweenWeatherChanges(fallback.getFallbackFloat("Weather_Hours_Between_Weather_Changes"))
    , mRainSpeed(fallback.getFallbackFloat("Weather_Precip_Gravity"))
    , mWeatherSettings()
    , mMasser("Masser", fallback)
    , mSecunda("Secunda", fallback)
    , mThunderFrequency(fallback.getFallbackFloat("Weather_Thunderstorm_Thunder_Frequency"))
    , mThunderThreshold(fallback.getFallbackFloat("Weather_Thunderstorm_Thunder_Threshold"))
    , mThunderSoundID0(fallback.getFallbackString("Weather_Thunderstorm_Thunder_Sound_ID_0"))
    , mThunderSoundID1(fallback.getFallbackString("Weather_Thunderstorm_Thunder_Sound_ID_1"))
    , mThunderSoundID2(fallback.getFallbackString("Weather_Thunderstorm_Thunder_Sound_ID_2"))
    , mThunderSoundID3(fallback.getFallbackString("Weather_Thunderstorm_Thunder_Sound_ID_3"))
    , mWindSpeed(0.f)
    , mIsStorm(false)
    , mStormDirection(0,1,0)
    , mThunderSoundDelay(0.25)
    , mThunderFlash(0)
    , mThunderChance(0)
    , mThunderChanceNeeded(50)
    , mCurrentRegion()
    , mTimePassed(0)
    , mFastForward(false)
    , mWeatherUpdateTime(mHoursBetweenWeatherChanges)
    , mTransitionFactor(0)
    , mCurrentWeather(0)
    , mNextWeather(0)
    , mQueuedWeather(0)
    , mRegions()
    , mResult()
    , mAmbientSound()
    , mPlayingSoundID()
{
    mWeatherSettings.reserve(10);
    addWeather("Clear", fallback);
    addWeather("Cloudy", fallback);
    addWeather("Foggy", fallback);
    addWeather("Overcast", fallback);
    addWeather("Rain", fallback, "rain");
    addWeather("Thunderstorm", fallback, "rain heavy");
    addWeather("Ashstorm", fallback, "ashstorm", "meshes\\ashcloud.nif");
    addWeather("Blight", fallback, "blight", "meshes\\blightcloud.nif");
    addWeather("Snow", fallback, "", "meshes\\snow.nif");
    addWeather("Blizzard", fallback, "BM Blizzard", "meshes\\blizzard.nif");

    Store<ESM::Region>::iterator it = store.get<ESM::Region>().begin();
    for(; it != store.get<ESM::Region>().end(); ++it)
    {
        std::string regionID = Misc::StringUtils::lowerCase(it->mId);
        mRegions.insert(std::make_pair(regionID, RegionWeather(*it)));
    }

    forceWeather(0);
}

WeatherManager::~WeatherManager()
{
    stopSounds();
}

void WeatherManager::changeWeather(const std::string& regionID, const unsigned int weatherID)
{
    // In Morrowind, this seems to have the following behavior, when applied to the current region:
    // - When there is no transition in progress, start transitioning to the new weather.
    // - If there is a transition in progress, queue up the transition and process it when the current one completes.
    // - If there is a transition in progress, and a queued transition, overwrite the queued transition.
    // - If multiple calls to ChangeWeather are made while paused (console up), only the last call will be used,
    //   meaning that if there was no transition in progress, only the last ChangeWeather will be processed.
    // If the region isn't current, Morrowind will store the new weather for the region in question.

    if(weatherID < mWeatherSettings.size())
    {
        std::string lowerCaseRegionID = Misc::StringUtils::lowerCase(regionID);
        std::map<std::string, RegionWeather>::iterator it = mRegions.find(lowerCaseRegionID);
        if(it != mRegions.end())
        {
            it->second.setWeather(weatherID);
            regionalWeatherChanged(it->first, it->second);
        }
    }
}

void WeatherManager::modRegion(const std::string& regionID, const std::vector<char>& chances)
{
    // Sets the region's probability for various weather patterns. Note that this appears to be saved permanently.
    // In Morrowind, this seems to have the following behavior when applied to the current region:
    // - If the region supports the current weather, no change in current weather occurs.
    // - If the region no longer supports the current weather, and there is no transition in progress, begin to
    //   transition to a new supported weather type.
    // - If the region no longer supports the current weather, and there is a transition in progress, queue a
    //   transition to a new supported weather type.

    std::string lowerCaseRegionID = Misc::StringUtils::lowerCase(regionID);
    std::map<std::string, RegionWeather>::iterator it = mRegions.find(lowerCaseRegionID);
    if(it != mRegions.end())
    {
        it->second.setChances(chances);
        regionalWeatherChanged(it->first, it->second);
    }
}

void WeatherManager::playerTeleported()
{
    // If the player teleports to an outdoors cell in a new region (for instance, by travelling), the weather needs to
    // be changed immediately, and any transitions for the previous region discarded.
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if(world->isCellExterior() || world->isCellQuasiExterior())
    {
        std::string playerRegion = Misc::StringUtils::lowerCase(world->getPlayerPtr().getCell()->getCell()->mRegion);
        std::map<std::string, RegionWeather>::iterator it = mRegions.find(playerRegion);
        if(it != mRegions.end() && playerRegion != mCurrentRegion)
        {
            mCurrentRegion = playerRegion;
            forceWeather(it->second.getWeather());
        }
    }
}

void WeatherManager::update(float duration, bool paused)
{
    MWWorld::Ptr player = MWMechanics::getPlayer();
    MWBase::World& world = *MWBase::Environment::get().getWorld();
    TimeStamp time = world.getTimeStamp();

    if(!paused)
    {
        // Add new transitions when either the player's current external region changes.
        std::string playerRegion = Misc::StringUtils::lowerCase(player.getCell()->getCell()->mRegion);
        if(updateWeatherTime() || updateWeatherRegion(playerRegion))
        {
            std::map<std::string, RegionWeather>::iterator it = mRegions.find(mCurrentRegion);
            if(it != mRegions.end())
            {
                addWeatherTransition(it->second.getWeather());
            }
        }

        updateWeatherTransitions(duration);
    }

    const bool exterior = (world.isCellExterior() || world.isCellQuasiExterior());
    if(!exterior)
    {
        mRendering.setSkyEnabled(false);
        //mRendering->getSkyManager()->setLightningStrength(0.f);
        stopSounds();
        return;
    }

    calculateWeatherResult(time.getHour());

    mWindSpeed = mResult.mWindSpeed;
    mIsStorm = mResult.mIsStorm;

    if (mIsStorm)
    {
        osg::Vec3f playerPos (player.getRefData().getPosition().asVec3());
        osg::Vec3f redMountainPos (19950, 72032, 27831);

        mStormDirection = (playerPos - redMountainPos);
        mStormDirection.z() = 0;
        mStormDirection.normalize();
        mRendering.getSkyManager()->setStormDirection(mStormDirection);
    }

    mRendering.configureFog(mResult.mFogDepth, mResult.mFogColor);

    // disable sun during night
    if (time.getHour() >= mNightStart || time.getHour() <= mSunriseTime)
        mRendering.getSkyManager()->sunDisable();
    else
        mRendering.getSkyManager()->sunEnable();

    // Update the sun direction.  Run it east to west at a fixed angle from overhead.
    // The sun's speed at day and night may differ, since mSunriseTime and mNightStart
    // mark when the sun is level with the horizon.
    {
        // Shift times into a 24-hour window beginning at mSunriseTime...
        float adjustedHour = time.getHour();
        float adjustedNightStart = mNightStart;
        if ( time.getHour() < mSunriseTime )
            adjustedHour += 24.f;
        if ( mNightStart < mSunriseTime )
            adjustedNightStart += 24.f;

        const bool is_night = adjustedHour >= adjustedNightStart;
        const float dayDuration = adjustedNightStart - mSunriseTime;
        const float nightDuration = 24.f - dayDuration;

        double theta;
        if ( !is_night ) {
            theta = M_PI * (adjustedHour - mSunriseTime) / dayDuration;
        } else {
            theta = M_PI * (adjustedHour - adjustedNightStart) / nightDuration;
        }

        osg::Vec3f final(
            static_cast<float>(cos(theta)),
            -0.268f, // approx tan( -15 degrees )
            static_cast<float>(sin(theta)));
        mRendering.setSunDirection( final * -1 );
    }

    mRendering.getSkyManager()->setMasserState(mMasser.calculateState(time));
    mRendering.getSkyManager()->setSecundaState(mSecunda.calculateState(time));

    if (!paused)
    {
        if(mCurrentWeather == 5 && !inTransition())
        {
            if (mThunderFlash > 0)
            {
                // play the sound after a delay
                mThunderSoundDelay -= duration;
                if (mThunderSoundDelay <= 0)
                {
                    // pick a random sound
                    int sound = Misc::Rng::rollDice(4);
                    std::string* soundName = NULL;
                    if (sound == 0) soundName = &mThunderSoundID0;
                    else if (sound == 1) soundName = &mThunderSoundID1;
                    else if (sound == 2) soundName = &mThunderSoundID2;
                    else if (sound == 3) soundName = &mThunderSoundID3;
                    if (soundName)
                        MWBase::Environment::get().getSoundManager()->playSound(*soundName, 1.0, 1.0);
                    mThunderSoundDelay = 1000;
                }

                mThunderFlash -= duration;
                //if (mThunderFlash > 0)
                    //mRendering->getSkyManager()->setLightningStrength( mThunderFlash / mThunderThreshold );
                //else
                {
                    mThunderChanceNeeded = static_cast<float>(Misc::Rng::rollDice(100));
                    mThunderChance = 0;
                    //mRendering->getSkyManager()->setLightningStrength( 0.f );
                }
            }
            else
            {
                // no thunder active
                mThunderChance += duration*4; // chance increases by 4 percent every second
                if (mThunderChance >= mThunderChanceNeeded)
                {
                    mThunderFlash = mThunderThreshold;

                    //mRendering->getSkyManager()->setLightningStrength( mThunderFlash / mThunderThreshold );

                    mThunderSoundDelay = 0.25;
                }
            }
        }
        //else
            //mRendering->getSkyManager()->setLightningStrength(0.f);
    }

    mRendering.setAmbientColour(mResult.mAmbientColor);
    mRendering.setSunColour(mResult.mSunColor);

    mRendering.getSkyManager()->setWeather(mResult);

    // Play sounds
    if (mPlayingSoundID != mResult.mAmbientLoopSoundID)
    {
        stopSounds();
        if (!mResult.mAmbientLoopSoundID.empty())
            mAmbientSound = MWBase::Environment::get().getSoundManager()->playSound(mResult.mAmbientLoopSoundID, 1.0, 1.0, MWBase::SoundManager::Play_TypeSfx, MWBase::SoundManager::Play_Loop);

        mPlayingSoundID = mResult.mAmbientLoopSoundID;
    }
    if (mAmbientSound.get())
        mAmbientSound->setVolume(mResult.mAmbientSoundVolume);
}

void WeatherManager::stopSounds()
{
    if (mAmbientSound.get())
    {
        MWBase::Environment::get().getSoundManager()->stopSound(mAmbientSound);
        mAmbientSound.reset();
        mPlayingSoundID.clear();
    }
}

float WeatherManager::getWindSpeed() const
{
    return mWindSpeed;
}

bool WeatherManager::isInStorm() const
{
    return mIsStorm;
}

osg::Vec3f WeatherManager::getStormDirection() const
{
    return mStormDirection;
}

void WeatherManager::advanceTime(double hours, bool incremental)
{
    // In Morrowind, when the player sleeps/waits, serves jail time, travels, or trains, all weather transitions are
    // immediately applied, regardless of whatever transition time might have been remaining.
    mTimePassed += hours;
    mFastForward = !incremental ? true : mFastForward;
}

unsigned int WeatherManager::getWeatherID() const
{
    return mCurrentWeather;
}

bool WeatherManager::isDark() const
{
    TimeStamp time = MWBase::Environment::get().getWorld()->getTimeStamp();
    bool exterior = (MWBase::Environment::get().getWorld()->isCellExterior()
                     || MWBase::Environment::get().getWorld()->isCellQuasiExterior());
    return exterior && (time.getHour() < mSunriseTime || time.getHour() > mNightStart - 1);
}

void WeatherManager::write(ESM::ESMWriter& writer, Loading::Listener& progress)
{
    ESM::WeatherState state;
    state.mCurrentRegion = mCurrentRegion;
    state.mTimePassed = mTimePassed;
    state.mFastForward = mFastForward;
    state.mWeatherUpdateTime = mWeatherUpdateTime;
    state.mTransitionFactor = mTransitionFactor;
    state.mCurrentWeather = mCurrentWeather;
    state.mNextWeather = mNextWeather;
    state.mQueuedWeather = mQueuedWeather;

    std::map<std::string, RegionWeather>::iterator it = mRegions.begin();
    for(; it != mRegions.end(); ++it)
    {
        state.mRegions.insert(std::make_pair(it->first, it->second));
    }

    writer.startRecord(ESM::REC_WTHR);
    state.save(writer);
    writer.endRecord(ESM::REC_WTHR);
}

bool WeatherManager::readRecord(ESM::ESMReader& reader, uint32_t type)
{
    if(ESM::REC_WTHR == type)
    {
        static const int oldestCompatibleSaveFormat = 2;
        if(reader.getFormat() < oldestCompatibleSaveFormat)
        {
            // Weather state isn't really all that important, so to preserve older save games, we'll just discard the
            // older weather records, rather than fail to handle the record.
            reader.skipRecord();
        }
        else
        {
            ESM::WeatherState state;
            state.load(reader);

            mCurrentRegion.swap(state.mCurrentRegion);
            mTimePassed = state.mTimePassed;
            mFastForward = state.mFastForward;
            mWeatherUpdateTime = state.mWeatherUpdateTime;
            mTransitionFactor = state.mTransitionFactor;
            mCurrentWeather = state.mCurrentWeather;
            mNextWeather = state.mNextWeather;
            mQueuedWeather = state.mQueuedWeather;

            mRegions.clear();
            std::map<std::string, ESM::RegionWeatherState>::iterator it = state.mRegions.begin();
            if(it == state.mRegions.end())
            {
                // When loading an imported save, the region modifiers aren't currently being set, so just reset them.
                importRegions();
            }
            else
            {
                for(; it != state.mRegions.end(); ++it)
                {
                    mRegions.insert(std::make_pair(it->first, RegionWeather(it->second)));
                }
            }
        }

        return true;
    }

    return false;
}

void WeatherManager::clear()
{
    stopSounds();

    mThunderFlash = 0.0;
    mThunderChance = 0.0;
    mThunderChanceNeeded = 50.0;

    mCurrentRegion = "";
    mTimePassed = 0.0f;
    mWeatherUpdateTime = 0.0f;
    forceWeather(0);
    mRegions.clear();
    importRegions();
}

inline void WeatherManager::addWeather(const std::string& name,
                                       const MWWorld::Fallback& fallback,
                                       const std::string& ambientLoopSoundID,
                                       const std::string& particleEffect)
{
    static const float fStromWindSpeed = mStore.get<ESM::GameSetting>().find("fStromWindSpeed")->getFloat();

    Weather weather(name, fallback, fStromWindSpeed, mRainSpeed, ambientLoopSoundID, particleEffect);

    mWeatherSettings.push_back(weather);
}

inline void WeatherManager::importRegions()
{
    Store<ESM::Region>::iterator it = mStore.get<ESM::Region>().begin();
    for(; it != mStore.get<ESM::Region>().end(); ++it)
    {
        std::string regionID = Misc::StringUtils::lowerCase(it->mId);
        mRegions.insert(std::make_pair(regionID, RegionWeather(*it)));
    }
}

inline void WeatherManager::regionalWeatherChanged(const std::string& regionID, RegionWeather& region)
{
    // If the region is current, then add a weather transition for it.
    MWWorld::Ptr player = MWMechanics::getPlayer();
    if(player.isInCell())
    {
        std::string playerRegion = Misc::StringUtils::lowerCase(player.getCell()->getCell()->mRegion);
        if(!playerRegion.empty() && (playerRegion == regionID))
        {
            addWeatherTransition(region.getWeather());
        }
    }
}

inline bool WeatherManager::updateWeatherTime()
{
    mWeatherUpdateTime -= mTimePassed;
    mTimePassed = 0.0f;
    if(mWeatherUpdateTime <= 0.0f)
    {
        // Expire all regional weather, so that any call to getWeather() will return a new weather ID.
        std::map<std::string, RegionWeather>::iterator it = mRegions.begin();
        for(; it != mRegions.end(); ++it)
        {
            it->second.setWeather(invalidWeatherID);
        }

        mWeatherUpdateTime += mHoursBetweenWeatherChanges;

        return true;
    }

    return false;
}

inline bool WeatherManager::updateWeatherRegion(const std::string& playerRegion)
{
    if(!playerRegion.empty() && playerRegion != mCurrentRegion)
    {
        mCurrentRegion = playerRegion;

        return true;
    }

    return false;
}

inline void WeatherManager::updateWeatherTransitions(const float elapsedRealSeconds)
{
    // When a player chooses to train, wait, or serves jail time, any transitions will be fast forwarded to the last
    // weather type set, regardless of the remaining transition time.
    if(!mFastForward && inTransition())
    {
        const float delta = mWeatherSettings[mNextWeather].transitionDelta();
        mTransitionFactor -= elapsedRealSeconds * delta;
        if(mTransitionFactor <= 0.0f)
        {
            mCurrentWeather = mNextWeather;
            mNextWeather = mQueuedWeather;
            mQueuedWeather = invalidWeatherID;

            // We may have begun processing the queued transition, so we need to apply the remaining time towards it.
            if(inTransition())
            {
                const float newDelta = mWeatherSettings[mNextWeather].transitionDelta();
                const float remainingSeconds = -(mTransitionFactor / delta);
                mTransitionFactor = 1.0f - (remainingSeconds * newDelta);
            }
            else
            {
                mTransitionFactor = 0.0f;
            }
        }
    }
    else
    {
        if(mQueuedWeather != invalidWeatherID)
        {
            mCurrentWeather = mQueuedWeather;
        }
        else if(mNextWeather != invalidWeatherID)
        {
            mCurrentWeather = mNextWeather;
        }

        mNextWeather = invalidWeatherID;
        mQueuedWeather = invalidWeatherID;
        mFastForward = false;
    }
}

inline void WeatherManager::forceWeather(const int weatherID)
{
    mTransitionFactor = 0.0f;
    mCurrentWeather = weatherID;
    mNextWeather = invalidWeatherID;
    mQueuedWeather = invalidWeatherID;
}

inline bool WeatherManager::inTransition()
{
    return mNextWeather != invalidWeatherID;
}

inline void WeatherManager::addWeatherTransition(const int weatherID)
{
    // In order to work like ChangeWeather expects, this method begins transitioning to the new weather immediately if
    // no transition is in progress, otherwise it queues it to be transitioned.

    assert(weatherID >= 0 && static_cast<size_t>(weatherID) < mWeatherSettings.size());

    if(!inTransition() && (weatherID != mCurrentWeather))
    {
        mNextWeather = weatherID;
        mTransitionFactor = 1.0f;
    }
    else if(inTransition() && (weatherID != mNextWeather))
    {
        mQueuedWeather = weatherID;
    }
}

inline void WeatherManager::calculateWeatherResult(const float gameHour)
{
    if(!inTransition())
    {
        calculateResult(mCurrentWeather, gameHour);
    }
    else
    {
        calculateTransitionResult(1 - mTransitionFactor, gameHour);
    }
}

inline void WeatherManager::calculateResult(const int weatherID, const float gameHour)
{
    const Weather& current = mWeatherSettings[weatherID];

    mResult.mCloudTexture = current.mCloudTexture;
    mResult.mCloudBlendFactor = 0;
    mResult.mWindSpeed = current.mWindSpeed;
    mResult.mCloudSpeed = current.mCloudSpeed;
    mResult.mGlareView = current.mGlareView;
    mResult.mAmbientLoopSoundID = current.mAmbientLoopSoundID;
    mResult.mAmbientSoundVolume = 1.f;
    mResult.mEffectFade = 1.f;
    mResult.mSunColor = current.mSunDiscSunsetColor;

    mResult.mIsStorm = current.mIsStorm;

    mResult.mRainSpeed = current.mRainSpeed;
    mResult.mRainFrequency = current.mRainFrequency;

    mResult.mParticleEffect = current.mParticleEffect;
    mResult.mRainEffect = current.mRainEffect;

    mResult.mNight = (gameHour < mSunriseTime || gameHour > mNightStart - 1);

    mResult.mFogDepth = mResult.mNight ? current.mLandFogNightDepth : current.mLandFogDayDepth;

    // night
    if (gameHour <= mNightEnd || gameHour >= mNightStart + 1)
    {
        mResult.mFogColor = current.mFogNightColor;
        mResult.mAmbientColor = current.mAmbientNightColor;
        mResult.mSunColor = current.mSunNightColor;
        mResult.mSkyColor = current.mSkyNightColor;
        mResult.mNightFade = 1.f;
    }

    // sunrise
    else if (gameHour >= mNightEnd && gameHour <= mDayStart + 1)
    {
        if (gameHour <= mSunriseTime)
        {
            // fade in
            float advance = mSunriseTime - gameHour;
            float factor = advance / 0.5f;
            mResult.mFogColor = lerp(current.mFogSunriseColor, current.mFogNightColor, factor);
            mResult.mAmbientColor = lerp(current.mAmbientSunriseColor, current.mAmbientNightColor, factor);
            mResult.mSunColor = lerp(current.mSunSunriseColor, current.mSunNightColor, factor);
            mResult.mSkyColor = lerp(current.mSkySunriseColor, current.mSkyNightColor, factor);
            mResult.mNightFade = factor;
        }
        else //if (gameHour >= 6)
        {
            // fade out
            float advance = gameHour - mSunriseTime;
            float factor = advance / 3.f;
            mResult.mFogColor = lerp(current.mFogSunriseColor, current.mFogDayColor, factor);
            mResult.mAmbientColor = lerp(current.mAmbientSunriseColor, current.mAmbientDayColor, factor);
            mResult.mSunColor = lerp(current.mSunSunriseColor, current.mSunDayColor, factor);
            mResult.mSkyColor = lerp(current.mSkySunriseColor, current.mSkyDayColor, factor);
        }
    }

    // day
    else if (gameHour >= mDayStart + 1 && gameHour <= mDayEnd - 1)
    {
        mResult.mFogColor = current.mFogDayColor;
        mResult.mAmbientColor = current.mAmbientDayColor;
        mResult.mSunColor = current.mSunDayColor;
        mResult.mSkyColor = current.mSkyDayColor;
    }

    // sunset
    else if (gameHour >= mDayEnd - 1 && gameHour <= mNightStart + 1)
    {
        if (gameHour <= mDayEnd + 1)
        {
            // fade in
            float advance = (mDayEnd + 1) - gameHour;
            float factor = (advance / 2);
            mResult.mFogColor = lerp(current.mFogSunsetColor, current.mFogDayColor, factor);
            mResult.mAmbientColor = lerp(current.mAmbientSunsetColor, current.mAmbientDayColor, factor);
            mResult.mSunColor = lerp(current.mSunSunsetColor, current.mSunDayColor, factor);
            mResult.mSkyColor = lerp(current.mSkySunsetColor, current.mSkyDayColor, factor);
        }
        else //if (gameHour >= 19)
        {
            // fade out
            float advance = gameHour - (mDayEnd + 1);
            float factor = advance / 2.f;
            mResult.mFogColor = lerp(current.mFogSunsetColor, current.mFogNightColor, factor);
            mResult.mAmbientColor = lerp(current.mAmbientSunsetColor, current.mAmbientNightColor, factor);
            mResult.mSunColor = lerp(current.mSunSunsetColor, current.mSunNightColor, factor);
            mResult.mSkyColor = lerp(current.mSkySunsetColor, current.mSkyNightColor, factor);
            mResult.mNightFade = factor;
        }
    }
}

inline void WeatherManager::calculateTransitionResult(const float factor, const float gameHour)
{
    calculateResult(mCurrentWeather, gameHour);
    const MWRender::WeatherResult current = mResult;
    calculateResult(mNextWeather, gameHour);
    const MWRender::WeatherResult other = mResult;

    mResult.mCloudTexture = current.mCloudTexture;
    mResult.mNextCloudTexture = other.mCloudTexture;
    mResult.mCloudBlendFactor = mWeatherSettings[mNextWeather].cloudBlendFactor(factor);

    mResult.mFogColor = lerp(current.mFogColor, other.mFogColor, factor);
    mResult.mSunColor = lerp(current.mSunColor, other.mSunColor, factor);
    mResult.mSkyColor = lerp(current.mSkyColor, other.mSkyColor, factor);

    mResult.mAmbientColor = lerp(current.mAmbientColor, other.mAmbientColor, factor);
    mResult.mSunDiscColor = lerp(current.mSunDiscColor, other.mSunDiscColor, factor);
    mResult.mFogDepth = lerp(current.mFogDepth, other.mFogDepth, factor);
    mResult.mWindSpeed = lerp(current.mWindSpeed, other.mWindSpeed, factor);
    mResult.mCloudSpeed = lerp(current.mCloudSpeed, other.mCloudSpeed, factor);
    mResult.mGlareView = lerp(current.mGlareView, other.mGlareView, factor);
    mResult.mNightFade = lerp(current.mNightFade, other.mNightFade, factor);

    mResult.mNight = current.mNight;

    if(factor < 0.5)
    {
        mResult.mIsStorm = current.mIsStorm;
        mResult.mParticleEffect = current.mParticleEffect;
        mResult.mRainEffect = current.mRainEffect;
        mResult.mParticleEffect = current.mParticleEffect;
        mResult.mRainSpeed = current.mRainSpeed;
        mResult.mRainFrequency = current.mRainFrequency;
        mResult.mAmbientSoundVolume = 1-(factor*2);
        mResult.mEffectFade = mResult.mAmbientSoundVolume;
        mResult.mAmbientLoopSoundID = current.mAmbientLoopSoundID;
    }
    else
    {
        mResult.mIsStorm = other.mIsStorm;
        mResult.mParticleEffect = other.mParticleEffect;
        mResult.mRainEffect = other.mRainEffect;
        mResult.mParticleEffect = other.mParticleEffect;
        mResult.mRainSpeed = other.mRainSpeed;
        mResult.mRainFrequency = other.mRainFrequency;
        mResult.mAmbientSoundVolume = 2*(factor-0.5f);
        mResult.mEffectFade = mResult.mAmbientSoundVolume;
        mResult.mAmbientLoopSoundID = other.mAmbientLoopSoundID;
    }
}

