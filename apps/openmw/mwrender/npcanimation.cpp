#include "npcanimation.hpp"

#include <osg/UserDataContainer>
#include <osg/MatrixTransform>
#include <osg/BlendFunc>
#include <osg/Material>

#include <components/misc/rng.hpp>

#include <components/misc/resourcehelpers.hpp>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/attach.hpp>
#include <components/sceneutil/visitor.hpp>

#include <components/nifosg/nifloader.hpp> // TextKeyMapHolder

#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/class.hpp"

#include "../mwmechanics/npcstats.hpp"
#include "../mwmechanics/actorutil.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"

#include "camera.hpp"
#include "rotatecontroller.hpp"

namespace
{

std::string getVampireHead(const std::string& race, bool female)
{
    static std::map <std::pair<std::string,int>, const ESM::BodyPart* > sVampireMapping;

    std::pair<std::string, int> thisCombination = std::make_pair(race, int(female));

    if (sVampireMapping.find(thisCombination) == sVampireMapping.end())
    {
        const MWWorld::ESMStore &store = MWBase::Environment::get().getWorld()->getStore();
        const MWWorld::Store<ESM::BodyPart> &partStore = store.get<ESM::BodyPart>();
        for(MWWorld::Store<ESM::BodyPart>::iterator it = partStore.begin(); it != partStore.end(); ++it)
        {
            const ESM::BodyPart& bodypart = *it;
            if (!bodypart.mData.mVampire)
                continue;
            if (bodypart.mData.mType != ESM::BodyPart::MT_Skin)
                continue;
            if (bodypart.mData.mPart != ESM::BodyPart::MP_Head)
                continue;
            if (female != (bodypart.mData.mFlags & ESM::BodyPart::BPF_Female))
                continue;
            if (!Misc::StringUtils::ciEqual(bodypart.mRace, race))
                continue;
            sVampireMapping[thisCombination] = &*it;
        }
    }

    if (sVampireMapping.find(thisCombination) == sVampireMapping.end())
        sVampireMapping[thisCombination] = NULL;

    const ESM::BodyPart* bodyPart = sVampireMapping[thisCombination];
    if (!bodyPart)
        return std::string();
    return "meshes\\" + bodyPart->mModel;
}

}


namespace MWRender
{

class HeadAnimationTime : public SceneUtil::ControllerSource
{
private:
    MWWorld::Ptr mReference;
    float mTalkStart;
    float mTalkStop;
    float mBlinkStart;
    float mBlinkStop;

    float mBlinkTimer;

    bool mEnabled;

    float mValue;
private:
    void resetBlinkTimer();
public:
    HeadAnimationTime(MWWorld::Ptr reference);

    void updatePtr(const MWWorld::Ptr& updated);

    void update(float dt);

    void setEnabled(bool enabled);

    void setTalkStart(float value);
    void setTalkStop(float value);
    void setBlinkStart(float value);
    void setBlinkStop(float value);

    virtual float getValue(osg::NodeVisitor* nv);
};

// --------------------------------------------------------------------------------

/// Subclass RotateController to add a Z-offset for sneaking in first person mode.
/// @note We use inheritance instead of adding another controller, so that we do not have to compute the worldOrient twice.
/// @note Must be set on a MatrixTransform.
class NeckController : public RotateController
{
public:
    NeckController(osg::Node* relativeTo)
        : RotateController(relativeTo)
    {
    }

    void setOffset(osg::Vec3f offset)
    {
        mOffset = offset;
    }

    virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        osg::MatrixTransform* transform = static_cast<osg::MatrixTransform*>(node);
        osg::Matrix matrix = transform->getMatrix();

        osg::Quat worldOrient = getWorldOrientation(node);
        osg::Quat orient = worldOrient * mRotate * worldOrient.inverse() * matrix.getRotate();

        matrix.setRotate(orient);
        matrix.setTrans(matrix.getTrans() + worldOrient.inverse() * mOffset);

        transform->setMatrix(matrix);

        traverse(node,nv);
    }

private:
    osg::Vec3f mOffset;
};

// --------------------------------------------------------------------------------------------------------------

HeadAnimationTime::HeadAnimationTime(MWWorld::Ptr reference)
    : mReference(reference), mTalkStart(0), mTalkStop(0), mBlinkStart(0), mBlinkStop(0), mEnabled(true), mValue(0)
{
    resetBlinkTimer();
}

void HeadAnimationTime::updatePtr(const MWWorld::Ptr &updated)
{
    mReference = updated;
}

void HeadAnimationTime::setEnabled(bool enabled)
{
    mEnabled = enabled;
}

void HeadAnimationTime::resetBlinkTimer()
{
    mBlinkTimer = -(2.0f + Misc::Rng::rollDice(6));
}

void HeadAnimationTime::update(float dt)
{
    if (!mEnabled)
        return;

    if (MWBase::Environment::get().getSoundManager()->sayDone(mReference))
    {
        mBlinkTimer += dt;

        float duration = mBlinkStop - mBlinkStart;

        if (mBlinkTimer >= 0 && mBlinkTimer <= duration)
        {
            mValue = mBlinkStart + mBlinkTimer;
        }
        else
            mValue = mBlinkStop;

        if (mBlinkTimer > duration)
            resetBlinkTimer();
    }
    else
    {
        // FIXME: would be nice to hold on to the SoundPtr so we don't have to retrieve it every frame
        mValue = mTalkStart +
            (mTalkStop - mTalkStart) *
            std::min(1.f, MWBase::Environment::get().getSoundManager()->getSaySoundLoudness(mReference)*2); // Rescale a bit (most voices are not very loud)
    }
}

float HeadAnimationTime::getValue(osg::NodeVisitor*)
{
    return mValue;
}

void HeadAnimationTime::setTalkStart(float value)
{
    mTalkStart = value;
}

void HeadAnimationTime::setTalkStop(float value)
{
    mTalkStop = value;
}

void HeadAnimationTime::setBlinkStart(float value)
{
    mBlinkStart = value;
}

void HeadAnimationTime::setBlinkStop(float value)
{
    mBlinkStop = value;
}

// ----------------------------------------------------

static NpcAnimation::PartBoneMap createPartListMap()
{
    NpcAnimation::PartBoneMap result;
    result.insert(std::make_pair(ESM::PRT_Head, "Head"));
    result.insert(std::make_pair(ESM::PRT_Hair, "Head")); // note it uses "Head" as attach bone, but "Hair" as filter
    result.insert(std::make_pair(ESM::PRT_Neck, "Neck"));
    result.insert(std::make_pair(ESM::PRT_Cuirass, "Chest"));
    result.insert(std::make_pair(ESM::PRT_Groin, "Groin"));
    result.insert(std::make_pair(ESM::PRT_Skirt, "Groin"));
    result.insert(std::make_pair(ESM::PRT_RHand, "Right Hand"));
    result.insert(std::make_pair(ESM::PRT_LHand, "Left Hand"));
    result.insert(std::make_pair(ESM::PRT_RWrist, "Right Wrist"));
    result.insert(std::make_pair(ESM::PRT_LWrist, "Left Wrist"));
    result.insert(std::make_pair(ESM::PRT_Shield, "Shield Bone"));
    result.insert(std::make_pair(ESM::PRT_RForearm, "Right Forearm"));
    result.insert(std::make_pair(ESM::PRT_LForearm, "Left Forearm"));
    result.insert(std::make_pair(ESM::PRT_RUpperarm, "Right Upper Arm"));
    result.insert(std::make_pair(ESM::PRT_LUpperarm, "Left Upper Arm"));
    result.insert(std::make_pair(ESM::PRT_RFoot, "Right Foot"));
    result.insert(std::make_pair(ESM::PRT_LFoot, "Left Foot"));
    result.insert(std::make_pair(ESM::PRT_RAnkle, "Right Ankle"));
    result.insert(std::make_pair(ESM::PRT_LAnkle, "Left Ankle"));
    result.insert(std::make_pair(ESM::PRT_RKnee, "Right Knee"));
    result.insert(std::make_pair(ESM::PRT_LKnee, "Left Knee"));
    result.insert(std::make_pair(ESM::PRT_RLeg, "Right Upper Leg"));
    result.insert(std::make_pair(ESM::PRT_LLeg, "Left Upper Leg"));
    result.insert(std::make_pair(ESM::PRT_RPauldron, "Right Clavicle"));
    result.insert(std::make_pair(ESM::PRT_LPauldron, "Left Clavicle"));
    result.insert(std::make_pair(ESM::PRT_Weapon, "Weapon Bone"));
    result.insert(std::make_pair(ESM::PRT_Tail, "Tail"));
    return result;
}
const NpcAnimation::PartBoneMap NpcAnimation::sPartList = createPartListMap();

NpcAnimation::~NpcAnimation()
{
    if (!mListenerDisabled
            // No need to getInventoryStore() to reset, if none exists
            // This is to avoid triggering the listener via ensureCustomData()->autoEquip()->fireEquipmentChanged()
            // all from within this destructor. ouch!
           && mPtr.getRefData().getCustomData() && mPtr.getClass().getInventoryStore(mPtr).getListener() == this)
        mPtr.getClass().getInventoryStore(mPtr).setListener(NULL, mPtr);
}

NpcAnimation::NpcAnimation(const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode, Resource::ResourceSystem* resourceSystem, bool disableListener, bool disableSounds, ViewMode viewMode)
  : Animation(ptr, parentNode, resourceSystem),
    mListenerDisabled(disableListener),
    mViewMode(viewMode),
    mShowWeapons(false),
    mShowCarriedLeft(true),
    mNpcType(Type_Normal),
    mAlpha(1.f),
    mSoundsDisabled(disableSounds)
{
    mNpc = mPtr.get<ESM::NPC>()->mBase;

    mHeadAnimationTime = boost::shared_ptr<HeadAnimationTime>(new HeadAnimationTime(mPtr));
    mWeaponAnimationTime = boost::shared_ptr<WeaponAnimationTime>(new WeaponAnimationTime(this));

    for(size_t i = 0;i < ESM::PRT_Count;i++)
    {
        mPartslots[i] = -1;  //each slot is empty
        mPartPriorities[i] = 0;
    }

    updateNpcBase();

    if (!disableListener)
        mPtr.getClass().getInventoryStore(mPtr).setListener(this, mPtr);
}

void NpcAnimation::setViewMode(NpcAnimation::ViewMode viewMode)
{
    assert(viewMode != VM_HeadOnly);
    if(mViewMode == viewMode) 
        return;

    mViewMode = viewMode;
    rebuild();
}

void NpcAnimation::rebuild()
{
    updateNpcBase();

    MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(mPtr);
}

int NpcAnimation::getSlot(const osg::NodePath &path) const
{
    for (int i=0; i<ESM::PRT_Count; ++i)
    {
        PartHolderPtr part = mObjectParts[i];
        if (!part.get())
            continue;
        if (std::find(path.begin(), path.end(), part->getNode().get()) != path.end())
        {
            return mPartslots[i];
        }
    }
    return -1;
}

void NpcAnimation::updateNpcBase()
{
    clearAnimSources();

    const MWWorld::ESMStore &store = MWBase::Environment::get().getWorld()->getStore();
    const ESM::Race *race = store.get<ESM::Race>().find(mNpc->mRace);
    bool isWerewolf = (mNpcType == Type_Werewolf);
    bool isVampire = (mNpcType == Type_Vampire);

    if (isWerewolf)
    {
        mHeadModel = "meshes\\" + store.get<ESM::BodyPart>().find("WerewolfHead")->mModel;
        mHairModel = "meshes\\" + store.get<ESM::BodyPart>().find("WerewolfHair")->mModel;
    }
    else
    {
        mHeadModel = "";
        if (isVampire) // FIXME: fall back to regular head when getVampireHead fails?
            mHeadModel = getVampireHead(mNpc->mRace, mNpc->mFlags & ESM::NPC::Female);
        else if (!mNpc->mHead.empty())
        {
            const ESM::BodyPart* bp = store.get<ESM::BodyPart>().search(mNpc->mHead);
            if (bp)
                mHeadModel = "meshes\\" + bp->mModel;
            else
                std::cerr << "Failed to load body part '" << mNpc->mHead << "'" << std::endl;
        }

        mHairModel = "";
        if (!mNpc->mHair.empty())
        {
            const ESM::BodyPart* bp = store.get<ESM::BodyPart>().search(mNpc->mHair);
            if (bp)
                mHairModel = "meshes\\" + bp->mModel;
            else
                std::cerr << "Failed to load body part '" << mNpc->mHair << "'" << std::endl;
        }
    }

    bool isBeast = (race->mData.mFlags & ESM::Race::Beast) != 0;
    std::string smodel = (mViewMode != VM_FirstPerson) ?
                         (!isWerewolf ? !isBeast ? "meshes\\base_anim.nif"
                                                 : "meshes\\base_animkna.nif"
                                      : "meshes\\wolf\\skin.nif") :
                         (!isWerewolf ? !isBeast ? "meshes\\base_anim.1st.nif"
                                                 : "meshes\\base_animkna.1st.nif"
                                      : "meshes\\wolf\\skin.1st.nif");
    smodel = Misc::ResourceHelpers::correctActorModelPath(smodel, mResourceSystem->getVFS());

    setObjectRoot(smodel, true, true, false);

    if(mViewMode != VM_FirstPerson)
    {
        addAnimSource(smodel);
        if(!isWerewolf)
        {
            if(Misc::StringUtils::lowerCase(mNpc->mRace).find("argonian") != std::string::npos)
                addAnimSource("meshes\\xargonian_swimkna.nif");
            else if(!mNpc->isMale() && !isBeast)
                addAnimSource("meshes\\xbase_anim_female.nif");
            if(mNpc->mModel.length() > 0)
                addAnimSource("meshes\\x"+mNpc->mModel);
        }
    }
    else
    {
        if(isWerewolf)
            addAnimSource(smodel);
        else
        {
            // A bit counter-intuitive, but unlike third-person anims, it seems
            // beast races get both base_anim.1st.nif and base_animkna.1st.nif.
            addAnimSource("meshes\\xbase_anim.1st.nif");
            if(isBeast)
                addAnimSource("meshes\\xbase_animkna.1st.nif");
            if(!mNpc->isMale() && !isBeast)
                addAnimSource("meshes\\xbase_anim_female.1st.nif");
        }
    }

    for(size_t i = 0;i < ESM::PRT_Count;i++)
        removeIndividualPart((ESM::PartReferenceType)i);
    updateParts();

    mWeaponAnimationTime->updateStartTime();
}

void NpcAnimation::updateParts()
{
    if (!mObjectRoot.get())
        return;

    mAlpha = 1.f;
    const MWWorld::Class &cls = mPtr.getClass();

    NpcType curType = Type_Normal;
    if (cls.getCreatureStats(mPtr).getMagicEffects().get(ESM::MagicEffect::Vampirism).getMagnitude() > 0)
        curType = Type_Vampire;
    if (cls.getNpcStats(mPtr).isWerewolf())
        curType = Type_Werewolf;

    if (curType != mNpcType)
    {
        mNpcType = curType;
        rebuild();
        return;
    }

    static const struct {
        int mSlot;
        int mBasePriority;
    } slotlist[] = {
        // FIXME: Priority is based on the number of reserved slots. There should be a better way.
        { MWWorld::InventoryStore::Slot_Robe,         12 },
        { MWWorld::InventoryStore::Slot_Skirt,         3 },
        { MWWorld::InventoryStore::Slot_Helmet,        0 },
        { MWWorld::InventoryStore::Slot_Cuirass,       0 },
        { MWWorld::InventoryStore::Slot_Greaves,       0 },
        { MWWorld::InventoryStore::Slot_LeftPauldron,  0 },
        { MWWorld::InventoryStore::Slot_RightPauldron, 0 },
        { MWWorld::InventoryStore::Slot_Boots,         0 },
        { MWWorld::InventoryStore::Slot_LeftGauntlet,  0 },
        { MWWorld::InventoryStore::Slot_RightGauntlet, 0 },
        { MWWorld::InventoryStore::Slot_Shirt,         0 },
        { MWWorld::InventoryStore::Slot_Pants,         0 },
        { MWWorld::InventoryStore::Slot_CarriedLeft,   0 },
        { MWWorld::InventoryStore::Slot_CarriedRight,  0 }
    };
    static const size_t slotlistsize = sizeof(slotlist)/sizeof(slotlist[0]);

    bool wasArrowAttached = (mAmmunition.get() != NULL);

    MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
    for(size_t i = 0;i < slotlistsize && mViewMode != VM_HeadOnly;i++)
    {
        MWWorld::ContainerStoreIterator store = inv.getSlot(slotlist[i].mSlot);

        removePartGroup(slotlist[i].mSlot);

        if(store == inv.end())
            continue;

        if(slotlist[i].mSlot == MWWorld::InventoryStore::Slot_Helmet)
            removeIndividualPart(ESM::PRT_Hair);

        int prio = 1;
        bool enchantedGlow = !store->getClass().getEnchantment(*store).empty();
        osg::Vec4f glowColor = getEnchantmentColor(*store);
        if(store->getTypeName() == typeid(ESM::Clothing).name())
        {
            prio = ((slotlist[i].mBasePriority+1)<<1) + 0;
            const ESM::Clothing *clothes = store->get<ESM::Clothing>()->mBase;
            addPartGroup(slotlist[i].mSlot, prio, clothes->mParts.mParts, enchantedGlow, &glowColor);
        }
        else if(store->getTypeName() == typeid(ESM::Armor).name())
        {
            prio = ((slotlist[i].mBasePriority+1)<<1) + 1;
            const ESM::Armor *armor = store->get<ESM::Armor>()->mBase;
            addPartGroup(slotlist[i].mSlot, prio, armor->mParts.mParts, enchantedGlow, &glowColor);
        }

        if(slotlist[i].mSlot == MWWorld::InventoryStore::Slot_Robe)
        {
            ESM::PartReferenceType parts[] = {
                ESM::PRT_Groin, ESM::PRT_Skirt, ESM::PRT_RLeg, ESM::PRT_LLeg,
                ESM::PRT_RUpperarm, ESM::PRT_LUpperarm, ESM::PRT_RKnee, ESM::PRT_LKnee,
                ESM::PRT_RForearm, ESM::PRT_LForearm
            };
            size_t parts_size = sizeof(parts)/sizeof(parts[0]);
            for(size_t p = 0;p < parts_size;++p)
                reserveIndividualPart(parts[p], slotlist[i].mSlot, prio);
        }
        else if(slotlist[i].mSlot == MWWorld::InventoryStore::Slot_Skirt)
        {
            reserveIndividualPart(ESM::PRT_Groin, slotlist[i].mSlot, prio);
            reserveIndividualPart(ESM::PRT_RLeg, slotlist[i].mSlot, prio);
            reserveIndividualPart(ESM::PRT_LLeg, slotlist[i].mSlot, prio);
        }
    }

    if(mViewMode != VM_FirstPerson)
    {
        if(mPartPriorities[ESM::PRT_Head] < 1 && !mHeadModel.empty())
            addOrReplaceIndividualPart(ESM::PRT_Head, -1,1, mHeadModel);
        if(mPartPriorities[ESM::PRT_Hair] < 1 && mPartPriorities[ESM::PRT_Head] <= 1 && !mHairModel.empty())
            addOrReplaceIndividualPart(ESM::PRT_Hair, -1,1, mHairModel);
    }
    if(mViewMode == VM_HeadOnly)
        return;

    if(mPartPriorities[ESM::PRT_Shield] < 1)
    {
        MWWorld::ContainerStoreIterator store = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
        MWWorld::Ptr part;
        if(store != inv.end() && (part=*store).getTypeName() == typeid(ESM::Light).name())
        {
            const ESM::Light *light = part.get<ESM::Light>()->mBase;
            addOrReplaceIndividualPart(ESM::PRT_Shield, MWWorld::InventoryStore::Slot_CarriedLeft,
                                       1, "meshes\\"+light->mModel);
            addExtraLight(mObjectParts[ESM::PRT_Shield]->getNode()->asGroup(), light);
        }
    }

    showWeapons(mShowWeapons);
    showCarriedLeft(mShowCarriedLeft);

    // Remember body parts so we only have to search through the store once for each race/gender/viewmode combination
    static std::map< std::pair<std::string,int>,std::vector<const ESM::BodyPart*> > sRaceMapping;

    bool isWerewolf = (mNpcType == Type_Werewolf);
    int flags = (isWerewolf ? -1 : 0);
    if(!mNpc->isMale())
    {
        static const int Flag_Female      = 1<<0;
        flags |= Flag_Female;
    }
    if(mViewMode == VM_FirstPerson)
    {
        static const int Flag_FirstPerson = 1<<1;
        flags |= Flag_FirstPerson;
    }

    std::string race = (isWerewolf ? "werewolf" : Misc::StringUtils::lowerCase(mNpc->mRace));
    std::pair<std::string, int> thisCombination = std::make_pair(race, flags);
    if (sRaceMapping.find(thisCombination) == sRaceMapping.end())
    {
        typedef std::multimap<ESM::BodyPart::MeshPart,ESM::PartReferenceType> BodyPartMapType;
        static BodyPartMapType sBodyPartMap;
        if(sBodyPartMap.empty())
        {
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Neck, ESM::PRT_Neck));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Chest, ESM::PRT_Cuirass));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Groin, ESM::PRT_Groin));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Hand, ESM::PRT_RHand));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Hand, ESM::PRT_LHand));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Wrist, ESM::PRT_RWrist));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Wrist, ESM::PRT_LWrist));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Forearm, ESM::PRT_RForearm));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Forearm, ESM::PRT_LForearm));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Upperarm, ESM::PRT_RUpperarm));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Upperarm, ESM::PRT_LUpperarm));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Foot, ESM::PRT_RFoot));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Foot, ESM::PRT_LFoot));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Ankle, ESM::PRT_RAnkle));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Ankle, ESM::PRT_LAnkle));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Knee, ESM::PRT_RKnee));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Knee, ESM::PRT_LKnee));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Upperleg, ESM::PRT_RLeg));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Upperleg, ESM::PRT_LLeg));
            sBodyPartMap.insert(std::make_pair(ESM::BodyPart::MP_Tail, ESM::PRT_Tail));
        }

        std::vector<const ESM::BodyPart*> &parts = sRaceMapping[thisCombination];
        parts.resize(ESM::PRT_Count, NULL);

        const MWWorld::ESMStore &store = MWBase::Environment::get().getWorld()->getStore();
        const MWWorld::Store<ESM::BodyPart> &partStore = store.get<ESM::BodyPart>();
        for(MWWorld::Store<ESM::BodyPart>::iterator it = partStore.begin(); it != partStore.end(); ++it)
        {
            if(isWerewolf)
                break;
            const ESM::BodyPart& bodypart = *it;
            if (bodypart.mData.mFlags & ESM::BodyPart::BPF_NotPlayable)
                continue;
            if (bodypart.mData.mType != ESM::BodyPart::MT_Skin)
                continue;

            if (!Misc::StringUtils::ciEqual(bodypart.mRace, mNpc->mRace))
                continue;

            bool firstPerson = (bodypart.mId.size() >= 3)
                    && bodypart.mId[bodypart.mId.size()-3] == '1'
                    && bodypart.mId[bodypart.mId.size()-2] == 's'
                    && bodypart.mId[bodypart.mId.size()-1] == 't';
            if(firstPerson != (mViewMode == VM_FirstPerson))
            {
                if(mViewMode == VM_FirstPerson && (bodypart.mData.mPart == ESM::BodyPart::MP_Hand ||
                                                   bodypart.mData.mPart == ESM::BodyPart::MP_Wrist ||
                                                   bodypart.mData.mPart == ESM::BodyPart::MP_Forearm ||
                                                   bodypart.mData.mPart == ESM::BodyPart::MP_Upperarm))
                {
                    /* Allow 3rd person skins as a fallback for the arms if 1st person is missing. */
                    BodyPartMapType::const_iterator bIt = sBodyPartMap.lower_bound(BodyPartMapType::key_type(bodypart.mData.mPart));
                    while(bIt != sBodyPartMap.end() && bIt->first == bodypart.mData.mPart)
                    {
                        if(!parts[bIt->second])
                            parts[bIt->second] = &*it;
                        ++bIt;
                    }
                }
                continue;
            }

            if ((!mNpc->isMale()) != (bodypart.mData.mFlags & ESM::BodyPart::BPF_Female))
            {
                // Allow opposite gender's parts as fallback if parts for our gender are missing
                BodyPartMapType::const_iterator bIt = sBodyPartMap.lower_bound(BodyPartMapType::key_type(bodypart.mData.mPart));
                while(bIt != sBodyPartMap.end() && bIt->first == bodypart.mData.mPart)
                {
                    if(!parts[bIt->second])
                        parts[bIt->second] = &*it;
                    ++bIt;
                }
                continue;
            }

            BodyPartMapType::const_iterator bIt = sBodyPartMap.lower_bound(BodyPartMapType::key_type(bodypart.mData.mPart));
            while(bIt != sBodyPartMap.end() && bIt->first == bodypart.mData.mPart)
            {
                parts[bIt->second] = &*it;
                ++bIt;
            }
        }
    }

    const std::vector<const ESM::BodyPart*> &parts = sRaceMapping[thisCombination];
    for(int part = ESM::PRT_Neck; part < ESM::PRT_Count; ++part)
    {
        if(mPartPriorities[part] < 1)
        {
            const ESM::BodyPart* bodypart = parts[part];
            if(bodypart)
                addOrReplaceIndividualPart((ESM::PartReferenceType)part, -1, 1,
                                           "meshes\\"+bodypart->mModel);
        }
    }

    if (wasArrowAttached)
        attachArrow();
}

PartHolderPtr NpcAnimation::insertBoundedPart(const std::string& model, const std::string& bonename, const std::string& bonefilter, bool enchantedGlow, osg::Vec4f* glowColor)
{
    osg::ref_ptr<osg::Node> instance = mResourceSystem->getSceneManager()->createInstance(model);
    osg::ref_ptr<osg::Node> attached = SceneUtil::attach(instance, mObjectRoot, bonefilter, bonename);
    mResourceSystem->getSceneManager()->notifyAttached(attached);
    if (enchantedGlow)
        addGlow(attached, *glowColor);

    return PartHolderPtr(new PartHolder(attached));
}

osg::Vec3f NpcAnimation::runAnimation(float timepassed)
{    
    osg::Vec3f ret = Animation::runAnimation(timepassed);

    mHeadAnimationTime->update(timepassed);

    if (mFirstPersonNeckController)
    {
        mFirstPersonNeckController->setRotate(osg::Quat(mPtr.getRefData().getPosition().rot[0], osg::Vec3f(-1,0,0)));
        mFirstPersonNeckController->setOffset(mFirstPersonOffset);
    }

    WeaponAnimation::configureControllers(mPtr.getRefData().getPosition().rot[0]);

    return ret;
}

void NpcAnimation::removeIndividualPart(ESM::PartReferenceType type)
{
    mPartPriorities[type] = 0;
    mPartslots[type] = -1;

    mObjectParts[type].reset();
    if (!mSoundIds[type].empty() && !mSoundsDisabled)
    {
        MWBase::Environment::get().getSoundManager()->stopSound3D(mPtr, mSoundIds[type]);
        mSoundIds[type].clear();
    }
}

void NpcAnimation::reserveIndividualPart(ESM::PartReferenceType type, int group, int priority)
{
    if(priority > mPartPriorities[type])
    {
        removeIndividualPart(type);
        mPartPriorities[type] = priority;
        mPartslots[type] = group;
    }
}

void NpcAnimation::removePartGroup(int group)
{
    for(int i = 0; i < ESM::PRT_Count; i++)
    {
        if(mPartslots[i] == group)
            removeIndividualPart((ESM::PartReferenceType)i);
    }
}

bool NpcAnimation::addOrReplaceIndividualPart(ESM::PartReferenceType type, int group, int priority, const std::string &mesh, bool enchantedGlow, osg::Vec4f* glowColor)
{
    if(priority <= mPartPriorities[type])
        return false;

    removeIndividualPart(type);
    mPartslots[type] = group;
    mPartPriorities[type] = priority;
    try
    {
        const std::string& bonename = sPartList.at(type);
        // PRT_Hair seems to be the only type that breaks consistency and uses a filter that's different from the attachment bone
        const std::string bonefilter = (type == ESM::PRT_Hair) ? "hair" : bonename;
        mObjectParts[type] = insertBoundedPart(mesh, bonename, bonefilter, enchantedGlow, glowColor);
    }
    catch (std::exception& e)
    {
        std::cerr << "Error adding NPC part: " << e.what() << std::endl;
        return false;
    }

    if (!mSoundsDisabled)
    {
        MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        MWWorld::ContainerStoreIterator csi = inv.getSlot(group < 0 ? MWWorld::InventoryStore::Slot_Helmet : group);
        if (csi != inv.end())
        {
            mSoundIds[type] = csi->getClass().getSound(*csi);
            if (!mSoundIds[type].empty())
            {
                MWBase::Environment::get().getSoundManager()->playSound3D(mPtr, mSoundIds[type], 1.0f, 1.0f, MWBase::SoundManager::Play_TypeSfx,
                    MWBase::SoundManager::Play_Loop);
            }
        }
    }

    boost::shared_ptr<SceneUtil::ControllerSource> src;
    if (type == ESM::PRT_Head)
    {
        src = mHeadAnimationTime;

        osg::Node* node = mObjectParts[type]->getNode();
        if (node->getUserDataContainer())
        {
            for (unsigned int i=0; i<node->getUserDataContainer()->getNumUserObjects(); ++i)
            {
                osg::Object* obj = node->getUserDataContainer()->getUserObject(i);
                if (NifOsg::TextKeyMapHolder* keys = dynamic_cast<NifOsg::TextKeyMapHolder*>(obj))
                {
                    for (NifOsg::TextKeyMap::const_iterator it = keys->mTextKeys.begin(); it != keys->mTextKeys.end(); ++it)
                    {
                        if (Misc::StringUtils::ciEqual(it->second, "talk: start"))
                            mHeadAnimationTime->setTalkStart(it->first);
                        if (Misc::StringUtils::ciEqual(it->second, "talk: stop"))
                            mHeadAnimationTime->setTalkStop(it->first);
                        if (Misc::StringUtils::ciEqual(it->second, "blink: start"))
                            mHeadAnimationTime->setBlinkStart(it->first);
                        if (Misc::StringUtils::ciEqual(it->second, "blink: stop"))
                            mHeadAnimationTime->setBlinkStop(it->first);
                    }

                    break;
                }
            }
        }
    }
    else if (type == ESM::PRT_Weapon)
        src = mWeaponAnimationTime;
    else
        src.reset(new NullAnimationTime);

    SceneUtil::AssignControllerSourcesVisitor assignVisitor(src);
    mObjectParts[type]->getNode()->accept(assignVisitor);

    return true;
}

void NpcAnimation::addPartGroup(int group, int priority, const std::vector<ESM::PartReference> &parts, bool enchantedGlow, osg::Vec4f* glowColor)
{
    const MWWorld::ESMStore &store = MWBase::Environment::get().getWorld()->getStore();
    const MWWorld::Store<ESM::BodyPart> &partStore = store.get<ESM::BodyPart>();

    const char *ext = (mViewMode == VM_FirstPerson) ? ".1st" : "";
    std::vector<ESM::PartReference>::const_iterator part(parts.begin());
    for(;part != parts.end();++part)
    {
        const ESM::BodyPart *bodypart = 0;
        if(!mNpc->isMale() && !part->mFemale.empty())
        {
            bodypart = partStore.search(part->mFemale+ext);
            if(!bodypart && mViewMode == VM_FirstPerson)
            {
                bodypart = partStore.search(part->mFemale);
                if(bodypart && !(bodypart->mData.mPart == ESM::BodyPart::MP_Hand ||
                                 bodypart->mData.mPart == ESM::BodyPart::MP_Wrist ||
                                 bodypart->mData.mPart == ESM::BodyPart::MP_Forearm ||
                                 bodypart->mData.mPart == ESM::BodyPart::MP_Upperarm))
                    bodypart = NULL;
            }
            else if (!bodypart)
                std::cerr << "Failed to find body part '" << part->mFemale << "'" << std::endl;
        }
        if(!bodypart && !part->mMale.empty())
        {
            bodypart = partStore.search(part->mMale+ext);
            if(!bodypart && mViewMode == VM_FirstPerson)
            {
                bodypart = partStore.search(part->mMale);
                if(bodypart && !(bodypart->mData.mPart == ESM::BodyPart::MP_Hand ||
                                 bodypart->mData.mPart == ESM::BodyPart::MP_Wrist ||
                                 bodypart->mData.mPart == ESM::BodyPart::MP_Forearm ||
                                 bodypart->mData.mPart == ESM::BodyPart::MP_Upperarm))
                    bodypart = NULL;
            }
            else if (!bodypart)
                std::cerr << "Failed to find body part '" << part->mMale << "'" << std::endl;
        }

        if(bodypart)
            addOrReplaceIndividualPart((ESM::PartReferenceType)part->mPart, group, priority, "meshes\\"+bodypart->mModel, enchantedGlow, glowColor);
        else
            reserveIndividualPart((ESM::PartReferenceType)part->mPart, group, priority);
    }
}

void NpcAnimation::addControllers()
{
    Animation::addControllers();

    mFirstPersonNeckController = NULL;
    WeaponAnimation::deleteControllers();

    if (mViewMode == VM_FirstPerson)
    {
        NodeMap::iterator found = mNodeMap.find("bip01 neck");
        if (found != mNodeMap.end() && dynamic_cast<osg::MatrixTransform*>(found->second.get()))
        {
            osg::Node* node = found->second;
            mFirstPersonNeckController = new NeckController(mObjectRoot.get());
            node->addUpdateCallback(mFirstPersonNeckController);
            mActiveControllers.insert(std::make_pair(node, mFirstPersonNeckController));
        }
    }
    else if (mViewMode == VM_Normal)
    {
        WeaponAnimation::addControllers(mNodeMap, mActiveControllers, mObjectRoot.get());
    }
}

void NpcAnimation::showWeapons(bool showWeapon)
{
    mShowWeapons = showWeapon;
    if(showWeapon)
    {
        MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        MWWorld::ContainerStoreIterator weapon = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        if(weapon != inv.end())
        {
            osg::Vec4f glowColor = getEnchantmentColor(*weapon);
            std::string mesh = weapon->getClass().getModel(*weapon);
            addOrReplaceIndividualPart(ESM::PRT_Weapon, MWWorld::InventoryStore::Slot_CarriedRight, 1,
                                       mesh, !weapon->getClass().getEnchantment(*weapon).empty(), &glowColor);

            // Crossbows start out with a bolt attached
            if (weapon->getTypeName() == typeid(ESM::Weapon).name() &&
                    weapon->get<ESM::Weapon>()->mBase->mData.mType == ESM::Weapon::MarksmanCrossbow)
            {
                MWWorld::ContainerStoreIterator ammo = inv.getSlot(MWWorld::InventoryStore::Slot_Ammunition);
                if (ammo != inv.end() && ammo->get<ESM::Weapon>()->mBase->mData.mType == ESM::Weapon::Bolt)
                    attachArrow();
                else
                    mAmmunition.reset();
            }
            else
                mAmmunition.reset();
        }
    }
    else
    {
        removeIndividualPart(ESM::PRT_Weapon);
    }
    mAlpha = 1.f;
}

void NpcAnimation::showCarriedLeft(bool show)
{
    mShowCarriedLeft = show;
    MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
    MWWorld::ContainerStoreIterator iter = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
    if(show && iter != inv.end())
    {
        osg::Vec4f glowColor = getEnchantmentColor(*iter);
        std::string mesh = iter->getClass().getModel(*iter);
        if (addOrReplaceIndividualPart(ESM::PRT_Shield, MWWorld::InventoryStore::Slot_CarriedLeft, 1,
                                   mesh, !iter->getClass().getEnchantment(*iter).empty(), &glowColor))
        {
            if (iter->getTypeName() == typeid(ESM::Light).name())
                addExtraLight(mObjectParts[ESM::PRT_Shield]->getNode()->asGroup(), iter->get<ESM::Light>()->mBase);
        }
    }
    else
        removeIndividualPart(ESM::PRT_Shield);
}

void NpcAnimation::attachArrow()
{
    WeaponAnimation::attachArrow(mPtr);
}

void NpcAnimation::releaseArrow(float attackStrength)
{
    WeaponAnimation::releaseArrow(mPtr, attackStrength);
}

osg::Group* NpcAnimation::getArrowBone()
{
    PartHolderPtr part = mObjectParts[ESM::PRT_Weapon];
    if (!part)
        return NULL;

    SceneUtil::FindByNameVisitor findVisitor ("ArrowBone");
    part->getNode()->accept(findVisitor);

    return findVisitor.mFoundNode;
}

osg::Node* NpcAnimation::getWeaponNode()
{
    PartHolderPtr part = mObjectParts[ESM::PRT_Weapon];
    if (!part)
        return NULL;
    return part->getNode();
}

Resource::ResourceSystem* NpcAnimation::getResourceSystem()
{
    return mResourceSystem;
}

void NpcAnimation::permanentEffectAdded(const ESM::MagicEffect *magicEffect, bool isNew, bool playSound)
{
    // During first auto equip, we don't play any sounds.
    // Basically we don't want sounds when the actor is first loaded,
    // the items should appear as if they'd always been equipped.
    if (playSound)
    {
        static const std::string schools[] = {
            "alteration", "conjuration", "destruction", "illusion", "mysticism", "restoration"
        };

        MWBase::SoundManager *sndMgr = MWBase::Environment::get().getSoundManager();
        if(!magicEffect->mHitSound.empty())
            sndMgr->playSound3D(mPtr, magicEffect->mHitSound, 1.0f, 1.0f);
        else
            sndMgr->playSound3D(mPtr, schools[magicEffect->mData.mSchool]+" hit", 1.0f, 1.0f);
    }

    if (!magicEffect->mHit.empty())
    {
        const ESM::Static* castStatic = MWBase::Environment::get().getWorld()->getStore().get<ESM::Static>().find (magicEffect->mHit);
        bool loop = (magicEffect->mData.mFlags & ESM::MagicEffect::ContinuousVfx) != 0;
        // Don't play particle VFX unless the effect is new or it should be looping.
        if (isNew || loop)
            addEffect("meshes\\" + castStatic->mModel, magicEffect->mIndex, loop, "");
    }
}

void NpcAnimation::setAlpha(float alpha)
{
    if (alpha == mAlpha)
        return;
    mAlpha = alpha;

    if (alpha != 1.f)
    {
        osg::StateSet* stateset (new osg::StateSet);

        osg::BlendFunc* blendfunc (new osg::BlendFunc);
        stateset->setAttributeAndModes(blendfunc, osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);

        // FIXME: overriding diffuse/ambient/emissive colors
        osg::Material* material (new osg::Material);
        material->setColorMode(osg::Material::OFF);
        material->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1,1,1,alpha));
        material->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1,1,1,1));
        stateset->setAttributeAndModes(material, osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);

        stateset->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
        stateset->setRenderBinMode(osg::StateSet::OVERRIDE_RENDERBIN_DETAILS);
        stateset->setNestRenderBins(false);
        mObjectRoot->setStateSet(stateset);
    }
    else
    {
        mObjectRoot->setStateSet(NULL);
    }
}

void NpcAnimation::enableHeadAnimation(bool enable)
{
    mHeadAnimationTime->setEnabled(enable);
}

void NpcAnimation::setWeaponGroup(const std::string &group)
{
    mWeaponAnimationTime->setGroup(group);
}

void NpcAnimation::equipmentChanged()
{
    updateParts();
}

void NpcAnimation::setVampire(bool vampire)
{
    if (mNpcType == Type_Werewolf) // we can't have werewolf vampires, can we
        return;
    if ((mNpcType == Type_Vampire) != vampire)
    {
        if (mPtr == MWMechanics::getPlayer())
            MWBase::Environment::get().getWorld()->reattachPlayerCamera();
        else
            rebuild();
    }
}

void NpcAnimation::setFirstPersonOffset(const osg::Vec3f &offset)
{
    mFirstPersonOffset = offset;
}

void NpcAnimation::updatePtr(const MWWorld::Ptr &updated)
{
    Animation::updatePtr(updated);
    mHeadAnimationTime->updatePtr(updated);
}

}
