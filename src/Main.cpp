#include "xbyak/xbyak.h"
#include "nlohmann/json.hpp"
using namespace SKSE;
using namespace SKSE::log;
using namespace SKSE::stl;

void InitializeLogging() {
    auto path = log_directory();
    if (!path) {
        report_and_fail("Unable to lookup SKSE logs directory.");
    }
    *path /= PluginDeclaration::GetSingleton()->GetName();
    *path += L".log";

    std::shared_ptr<spdlog::logger> log;
    if (IsDebuggerPresent()) {
        log = std::make_shared<spdlog::logger>(
            "Global", std::make_shared<spdlog::sinks::msvc_sink_mt>());
    }
    else {
        log = std::make_shared<spdlog::logger>(
            "Global", std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true));
    }


#ifndef NDEBUG
    const auto level = spdlog::level::trace;
#else
    const auto level = GetKeyState(VK_RCONTROL) & 0x800 ? spdlog::level::debug: spdlog::level::info;
#endif


    log->set_level(level);
    log->flush_on(level);

    spdlog::set_default_logger(std::move(log));
    //spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [%t] [%s:%#] %v");
    spdlog::set_pattern("%s(%#): [%^%l%$] %v"s);


#ifdef NDEBUG
    if (spdlog::level::debug == level) {
        logger::debug("debug logger in release enabled.");
    }
#endif
    
}


namespace RE
{
    class FloatSetting
    {
    private:
        uintptr_t _vtable = RE::VTABLE_Setting[0].address(); //0x00

    public:
        float			currValue;							// 0x08
        mutable float	prevValue;							// 0x0C
        const char* name;									// 0x10

        float GetValue() const { return currValue; }

        void Update() const { prevValue = currValue; }

        FloatSetting(const char* a_name, float a_value)
        {
            name = a_name;
            currValue = a_value;
            prevValue = a_value;
        }

        operator float() { return currValue; }
        operator RE::Setting* () { return reinterpret_cast<RE::Setting*>(this); }
    };
    static_assert(sizeof(FloatSetting) == 0x18);
}


int IsCallOrJump(uintptr_t addr)
{
    //0x15 0xE8//These are calls, represented by negative numbers
    //0x25 0xE9//These are jumps, represented by positive numbers.
    //And zero represent it being neither.

    if (addr)
    {
        auto first_byte = reinterpret_cast<uint8_t*>(addr);

        switch (*first_byte)
        {
        case 0x15:
        case 0xE8:
            return -1;

        case 0x25:
        case 0xE9:
            return 1;

        }
    }

    return 0;
}

//SE: 0x692390, AE: 0x6CC2B0, VR: ???
REL::Relocation<void(RE::CachedValues*, RE::ActorValue)> ResetCache{ REL::RelocationID(39159, 40225, 0) };


using SettingFlag = RE::EffectSetting::EffectSettingData::Flag;


RE::FloatSetting minSpeed{ "fMinWeaponSpeed", 0.5f };
RE::FloatSetting capSpeed{ "fWeaponSpeedCap", 2.f };
//RE::FloatSetting lowCapSpeed{ "fLowWeaponSpeedCap", 2.f };
RE::FloatSetting speedTaper{ "fWeaponSpeedTaper", 0.2f };
RE::FloatSetting maxSpeed{ "fMaxWeaponSpeed", 3.f };


//This value is used to ensure that whatever value the magnitude is able to go into
RE::FloatSetting magnitudeComparison{ "fMagnitudeComparison", 10000.f };


float GetEffectiveSpeed(RE::ActorValueOwner* target, bool right)
{
    float speed;

    if (right)
        speed = target->GetActorValue(RE::ActorValue::kWeaponSpeedMult);
    else
        speed = target->GetActorValue(RE::ActorValue::kLeftWeaponSpeedMultiply);

    float max_speed = !maxSpeed.GetValue() ? std::numeric_limits<float>::infinity(): fmax(maxSpeed.GetValue(), 1.f);//If zero, no maximum
    float min_speed = fmax(minSpeed.GetValue(), 0.01f);//It has to have some impact. Should you be brutal enough, go for it.
    float speed_taper = fmin(speedTaper.GetValue(), 1.f);//Not allowed to exceed 1. Gets fucky if it does.
    float cap_speed = std::clamp(capSpeed.GetValue(), min_speed, max_speed);
   

    float low_cap_speed = 0.75f;

    if (speed <= min_speed)
        return min_speed;

    if (speed_taper <= 0)
        return cap_speed;


    //These can be the same function, just altered or something.
    if (speed > cap_speed)
    {
        float extra_speed = speed - cap_speed;

        float new_speed = cap_speed + sqrt(extra_speed) * pow(speed_taper, 1.0f / extra_speed);

        speed = new_speed;
        //speed = std::clamp(speed, minSpeed, capSpeed);
    }
    else if constexpr (false)// speed < low_cap_speed)
    {
        float extra_speed = low_cap_speed - speed;


        float new_speed = low_cap_speed - sqrt(extra_speed) / pow(speed_taper, 1.0f / extra_speed);

        speed = new_speed;
    }


    if (target->GetIsPlayerOwner() == true)
        logger::debug("max:{}, min:{}, tap:{}, h_cap:{}, l_cap:{} = spd:{}", max_speed, min_speed, speed_taper, cap_speed, low_cap_speed, speed);


    if (speed >= max_speed)
        return max_speed;



    return speed;
}



float GetEffectiveSpeedFromActor(RE::StaticFunctionTag*, RE::Actor* target, bool right) 
{ 
    if (!target) return 0.f; 
    return GetEffectiveSpeed(target->AsActorValueOwner(), right); 
}



//I'm thinking of implementing 2 things. First, a cap, then a taper, then a max. Maybe something for min. Basically, the lower it gets the more it
// creeps toward a max or min.

//write_call/write_branch
struct WeaponSpeedMultHook
{
	static void Patch()
	{
        constexpr bool is_write_branch = true;

        if constexpr (is_write_branch)
        {
            //SE: 0x3BE440, AE: 0x3D7FB0, VR: ???
            REL::Relocation<uintptr_t> WeaponSpeedHook{ REL::RelocationID(25851, 26417) };
            
            auto& trampoline = SKSE::GetTrampoline();

            trampoline.write_branch<5>(WeaponSpeedHook.address(), thunk1);
        }
        else
        {
            //SE: 0x71B670, AE: 0x758510, VR: ???
            REL::Relocation<uintptr_t> WeaponSpeedHook{ REL::RelocationID { 41694, 42779 }, 0x29 };

            auto& trampoline = SKSE::GetTrampoline();

            func = trampoline.write_call<5>(WeaponSpeedHook.address(), thunk1);

        }

        
		logger::info("WeaponSpeedMultHook complete...");
	}

	//This hook is so ununique btw, that I think I can just write branch this shit. Straight up.
	static float thunk(RE::ActorValueOwner* av_owner, RE::TESObjectWEAP* weap, bool is_left)
	{
		//SE: 0x2EFF868, AE: 0x2F99450
		//static uintptr_t fists = REL::RelocationID(514923, 401061).address();
        
        //The above crashed for me because I'm a fucking moron, so it's this now.
		static RE::TESObjectWEAP* fists = RE::TESForm::LookupByID<RE::TESObjectWEAP>(0x1F4);

		if (!weap)
			weap = fists;//reinterpret_cast<RE::TESObjectWEAP*>(fists);

		return func(av_owner, weap, is_left);
	}

    
    static float thunk1(RE::ActorValueOwner* av_owner, RE::TESObjectWEAP* weap, bool is_left)
    {
        static RE::TESObjectWEAP* fists = RE::TESForm::LookupByID<RE::TESObjectWEAP>(0x1F4);

        if (!weap)
            weap = fists;//reinterpret_cast<RE::TESObjectWEAP*>(fists);

        float speed = GetEffectiveSpeed(av_owner, !is_left);

        bool two_handed = weap->weaponData.animationType.any
        (
            //RE::WEAPON_TYPE::kCrossbow, 
            RE::WEAPON_TYPE::kTwoHandAxe, 
            RE::WEAPON_TYPE::kTwoHandSword
        );
        
        static RE::Setting* two_handed_speed_mult = RE::GameSettingCollection::GetSingleton()->GetSetting("fWeaponTwoHandedAnimationSpeedMult");

        if (!two_handed_speed_mult){
            two_handed_speed_mult = RE::GameSettingCollection::GetSingleton()->GetSetting("fWeaponTwoHandedAnimationSpeedMult");
        }
        else 
        {
            switch (*weap->weaponData.animationType)
            {
            //case RE::WEAPON_TYPE::kCrossbow:
            case RE::WEAPON_TYPE::kTwoHandAxe:
            case RE::WEAPON_TYPE::kTwoHandSword:
                float mult = two_handed_speed_mult->GetFloat();
                //logger::info("2handMult {}", mult);
                speed *= mult;
            }
        }
        
        speed *= weap->weaponData.speed;


        return speed;
    }


	static inline REL::Relocation<decltype(thunk)> func;
};

//3e1790+0x37f /EEA Base actor value function?
//void *a1, ActorValueInfoList? Probably. Scratch, it is.
//uint32 a2, ActorValueIndex
//char* a3, Name(I can figure out where the fuck to put the damn name with this
//int32 a4, Seems to be a type.
//              0 is an attribute, think health and such, regen values. Common theme seems to be the executable controls it.
//              1 is a skill, perk tree specifically perhaps, because it goes into things that don't have skills like werewolves.
//              2 is ai values I think
//              3 is resistance. Pretty easy.
//              5 is flag values maybe? all of the values do something when not 0, except detect life range.
//              6 is unknown, seems to be user defined stuff, but tele and absorb chance use it?
//int32 a5, VERY likely a series of flag values, to an enum not to my understanding.
//void* a6, The default function of the object. Seems to take float(*)(Unk-176 =>Actor, ActorValue)
//int32 a7, Unknown. Value is always 0.

//write_call
struct CreateActorValueInfoHook
{
    using DefaultValueFunc = float(uint64_t, RE::ActorValue);

    static void Patch()
    {
        //I'm going to make this into a rewrite function.
        //SE: 0x3E1790, AE: 0x3FC8E0, VR: ???
        REL::Relocation<uintptr_t> InitAVI{ REL::RelocationID{ 26574, 27232 } };

        auto& trampoline = SKSE::GetTrampoline();

        func[0] = trampoline.write_call<5>(InitAVI.address() + 0xE7F, thunk<0>);
        func[1] = trampoline.write_call<5>(InitAVI.address() + 0xEAA, thunk<1>);

        logger::info("CreateActorValueInfo complete...");
    }
    
    template<int I = 0>
    static RE::ActorValueInfo* thunk(RE::ActorValueList* list, RE::ActorValue id, const char* name, int32_t unk_type, int32_t unk_flags, void* a, int32_t unk_val)
    {
        unk_flags &= ~0x8000;//AND out the default to zero flag.
        unk_flags |= 0x10000;//OR in the default to 1 flag.
        //unk_flags |= 0x800;//OR in the clear flag.

        
        //logger::info("id {}, name {}", (int)id, name);
        return func[I](list, id, name, unk_type, unk_flags, a, unk_val);
    }
    
    
    

    static inline REL::Relocation<decltype(thunk<>)> func[2];
};


uint32_t& GetActorTag(RE::Actor* target, bool right)
{
    if (right)
        return target->pad1C;
    else
       return target->GetActorRuntimeData().pad0EC;
}

//inline uint32_t& GetActorTag(RE::Actor* target, RE::ActorValue av)
//{
//    return GetActorTag(target, av == RE::ActorValue::kWeaponSpeedMult);
//}

constexpr bool k_right = true;
constexpr bool k_left = false;

//The existence of this is solely to scream when I take it away.
constexpr size_t VR_CALLER = 0;


int HandleActorTag(RE::ValueModifierEffect* a_this, bool is_on, float value)
{
    if (!value)
        return 0;

    if (is_on)
    {
        //Disabled that for now because of no constructor.
        if (value < 1.f) //|| !a_this->pad86)
        {
            return 0;
        }
    }
    else
    {
        if (value > -1.f)//if value is returning a small sum or restoring a large decrement.
            return 0;
    }

    return value > 0 ? 1 : -1;
}


RE::Actor* GetTargetActor(RE::MagicTarget* target)
{
    if (target && target->MagicTargetIsActor()) {
        auto r = target->GetTargetStatsObject();
        if (r)
            return r->As<RE::Actor>();
    }

    return nullptr;
}

void correct(RE::Actor* t)
{

    static bool once = false;

    if (!once)
    {
        GetActorTag(t, k_right) = 0;
        GetActorTag(t, k_left) = 0;
        once = true;
    }
}


//Note, currently these adjustments don't work if the magnitude gets flipped. Need to account for that sort of situations.
// That will likely rest in handle actor tag. But due to the projects nature, I can add it any time the problem arises.

//make a get left right function, that way I can move the padding if need be.
//template both parameters maybe?
void HandleSpeedEffect(RE::ValueModifierEffect* a_this, float value, bool is_dual, bool is_on)
{
    //The effect is redundant
    //is_dual is if it's dual.
    // Is on declares that negative values are of no concern, they're intentional decreases. Off will need a flag. That flag will not be saved however.
    // that too goes in padding.

    //The value plucked for dual value modifer is the size of this + 98. That 4 past 94 being another multiplier value.

    //pad86 is the padding I need. I want to hook ValueModifierEffect__ctor_1405671F0 for it, so I can free up some space for any other padding use.

    //Note, due to lack of dealing with being inited from save, AND constructor hooks. this is woefully unprepared for a save.

    //BIG NOTE
    //When handling magnitude, use the base magnitude instead of the current value.

        RE::Actor* target = GetTargetActor(a_this->target);
        
        if (!target) {
            logger::debug("Null target");
            return;
        }
        switch (a_this->actorValue)
        {
        case RE::ActorValue::kWeaponSpeedMult:
            //target->pad1C += HandleActorTag(a_this, is_on, value);
            GetActorTag(target, k_right) += HandleActorTag(a_this, is_on, value);
            logger::debug("Right1st: {}, {}", value, GetActorTag(target, k_right));
            break;
        case RE::ActorValue::kLeftWeaponSpeedMultiply:
            //target->GetActorRuntimeData().pad0EC += HandleActorTag(a_this, is_on, value);
            GetActorTag(target, k_left) += HandleActorTag(a_this, is_on, value);
            logger::debug("Left1st: {}, {}", value, GetActorTag(target, k_left));
            break;
        default:
            logger::debug("{}", a_this->actorValue);
        }

        
        if (is_dual)
        {
            RE::EffectSetting* setting = a_this->GetBaseObject();
            
            
            float dual_mod = *stl::adjust_pointer<float>(a_this, 0x98);//GetDualMod(a_this);//

            logger::debug("dual mod, {}", dual_mod);

            switch (setting->data.secondaryAV)
            {
            case RE::ActorValue::kWeaponSpeedMult:
                //target->pad1C += HandleActorTag(a_this, is_on, value * dual_mod);
                GetActorTag(target, k_right) += HandleActorTag(a_this, is_on, value * dual_mod);
                logger::debug("Right2nd: {}, {}", value * dual_mod, GetActorTag(target, k_right));
                break;
            case RE::ActorValue::kLeftWeaponSpeedMultiply:
                //target->GetActorRuntimeData().pad0EC += HandleActorTag(a_this, is_on, value * dual_mod);
                GetActorTag(target, k_left) += HandleActorTag(a_this, is_on, value * dual_mod);
                logger::debug("Left2nd: {}, {}", value * dual_mod, GetActorTag(target, k_left));//Is 2 for some odd reason.
                break;
            }
        }

        
}


//NOTICE, to all vtable hooks, ValueAndConditionsEffect may be implemented soon, so might want to get my hooks into that.

//VTABLE
struct ValueEffectStartHook
{
    static void Patch()
    {
        func[0] = REL::Relocation<uintptr_t>{ RE::VTABLE_ValueModifierEffect[0] }.write_vfunc(20, thunk<0>);
        func[1] = REL::Relocation<uintptr_t>{ RE::VTABLE_DualValueModifierEffect[0] }.write_vfunc(20, thunk<1>);
        func[2] = REL::Relocation<uintptr_t>{ RE::VTABLE_AccumulatingValueModifierEffect[0] }.write_vfunc(20, thunk<2>);
        func[3] = REL::Relocation<uintptr_t>{ RE::VTABLE_PeakValueModifierEffect[0] }.write_vfunc(20, thunk<3>);
        func[4] = REL::Relocation<uintptr_t>{ RE::VTABLE_EnhanceWeaponEffect[0] }.write_vfunc(20, thunk<4>);
        
        logger::info("ValueEffectStartHook complete...");
    }


    template <int I = 0>
    static void thunk(RE::ValueModifierEffect* a_this)
    {
        func[I](a_this);
        

        if constexpr (I == 4)
        {
            if (a_this->flags.any(RE::ActiveEffect::Flag::kDispelled) == true) {
                //logger::info("enhance effect dispelled.");
                return;
            }
        }

        auto effect = a_this->effect;


        float alignment = a_this->value >= 0 ? 1 : -1;


        if (a_this->flags.all(RE::ActiveEffect::Flag::kRecovers) == true && 
            effect->baseEffect->data.flags.all(SettingFlag::kDetrimental) == false)
            //Redesign for it to use
            //return HandleSpeedEffect(a_this, a_this->value, I == 1, true);
            return HandleSpeedEffect(a_this, a_this->effect->GetMagnitude() * alignment, I == 1, true);

        return;

        switch (a_this->actorValue)
        {
        case RE::ActorValue::kWeaponSpeedMult:
        case RE::ActorValue::kLeftWeaponSpeedMultiply:
            break;
        default:
            return;
        }

        static constexpr std::string_view names[]
        {
            "ValueMod",
            "DualMod",
            "AccumMod",
            "PeakMod",
            "Enhance"
        };
        if constexpr (I == 1)
        {
            //is dual modifier, peak a bit into the base object for a second round of this function
        }

        logger::info("ON {}: {}", names[I], a_this->value);
    }




    static inline REL::Relocation<decltype(thunk<>)> func[5];
};

//VTABLE
struct ValueEffectFinishHook
{
    static void Patch()
    {
        func[0] = REL::Relocation<uintptr_t>{ RE::VTABLE_ValueModifierEffect[0] }.write_vfunc(21, thunk<0>);
        func[1] = REL::Relocation<uintptr_t>{ RE::VTABLE_DualValueModifierEffect[0] }.write_vfunc(21, thunk<1>);
        func[2] = REL::Relocation<uintptr_t>{ RE::VTABLE_AccumulatingValueModifierEffect[0] }.write_vfunc(21, thunk<2>);
        func[3] = REL::Relocation<uintptr_t>{ RE::VTABLE_PeakValueModifierEffect[0] }.write_vfunc(21, thunk<3>);
        func[4] = REL::Relocation<uintptr_t>{ RE::VTABLE_EnhanceWeaponEffect[0] }.write_vfunc(21, thunk<4>);

        logger::info("ValueEffectFinishHook complete...");
    }


    template <int I = 0>
    static void thunk(RE::ValueModifierEffect* a_this)
    {
        func[I](a_this);


        float alignment = a_this->value >= 0 ? 1 : -1;


        if (a_this->flags.all(RE::ActiveEffect::Flag::kRecovers) == true &&
            effect->baseEffect->data.flags.all(SettingFlag::kDetrimental) == false)
            //HandleSpeedEffect(a_this, a_this->value, I == 1, false);
            HandleSpeedEffect(a_this, a_this->effect->GetMagnitude() * alignment, I == 1, false);

        return;

        switch (a_this->actorValue)
        {
        case RE::ActorValue::kWeaponSpeedMult:
        case RE::ActorValue::kLeftWeaponSpeedMultiply:
            //Only allowed to proceed if it's recovers and the value is at or equal to 1.
            break;
        default:
            return;
        }

        static constexpr std::string_view names[]
        {
            "ValueMod",
            "DualMod",
            "AccumMod",
            "PeakMod",
            "Enhance"
        };
        if constexpr (I == 1)
        {
            //is dual modifier, peak a bit into the base object for a second round of this function
        }

        logger::info("OFF {}: {}", names[I], a_this->value);
    }




    static inline REL::Relocation<decltype(thunk<>)> func[5];
};


//write_branch
struct GetActorValueModifierHook
{
    static void Patch()
    {
        //Use variantID at some point pls.
        //auto hook_addr = REL::ID(37524 /*0x621350*/).address();
        auto hook_addr = REL::RelocationID(37524, 38469).address();//SE: 0x621350, AE: 0x658BD0, VR: ???
        auto return_addr = hook_addr + 0x6;
        //*
        struct Code : Xbyak::CodeGenerator
        {
            Code(uintptr_t ret_addr)
            {
                //AE/SE versions remain the same
                push(rbx);
                sub(rsp, 0x20);

                mov(rax, ret_addr);
                jmp(rax);
            }
        } static code{ return_addr };

        auto& trampoline = SKSE::GetTrampoline();

        //func = (uintptr_t)code.getCode();

        //trampoline.write_branch<5>(hook_addr, thunk);

        //return;
        auto placed_call = IsCallOrJump(hook_addr) > 0;

        auto place_query = trampoline.write_branch<5>(hook_addr, (uintptr_t)thunk);

        if (!placed_call)
            func = (uintptr_t)code.getCode();
        else
            func = place_query;

        logger::info("GetActorValueModifier Hook complete...");
        //*/
    }

    static float thunk(RE::Character* a_this, RE::ACTOR_VALUE_MODIFIER a2, RE::ActorValue a3)
    {
        float result = func(a_this, a2, a3);

        if (a2 != RE::ACTOR_VALUE_MODIFIER::kTemporary)
            return result;

        switch (a3)
        {
        case RE::ActorValue::kWeaponSpeedMult:
            //return result - a_this->pad1C;
            return result - GetActorTag(a_this, k_right);

        case RE::ActorValue::kLeftWeaponSpeedMultiply:
            //return result - a_this->GetActorRuntimeData().pad0EC;
            return result - GetActorTag(a_this, k_left);

        default:
            return result;
        }
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

//VTABLE
struct GetActorValueHook
{
    static void Patch()
    {
        //*
        REL::Relocation<uintptr_t> PlayerCharacter__Actor_VTable{ RE::VTABLE_PlayerCharacter[5] };
        REL::Relocation<uintptr_t> Character__Actor_VTable{ RE::VTABLE_Character[5] };

        func[0] = PlayerCharacter__Actor_VTable.write_vfunc(0x01, thunk<0>);
        func[1] = Character__Actor_VTable.write_vfunc(0x01, thunk<1>);

        logger::info("GetActorValueHook complete...");
    }

    template <int I>
    static float thunk(RE::ActorValueOwner* a_this, RE::ActorValue a2)
    {
        RE::Character* target = skyrim_cast<RE::Character*>(a_this);

        float result = func[I](a_this, a2);

        switch (a2)
        {
        case RE::ActorValue::kWeaponSpeedMult:
            //return result - target->pad1C;
            return result - GetActorTag(target, k_right);

        case RE::ActorValue::kLeftWeaponSpeedMultiply:
            //return result - target->GetActorRuntimeData().pad0EC;
            return result - GetActorTag(target, k_left);

        default:
            return result;
        }
    }



    static inline REL::Relocation<decltype(thunk<0>)> func[2];
    //static inline REL::Relocation<decltype(thunk)> func_;
};


//VTABLE
struct ValueEffectLoadGameHook
{
    static void Patch()
    {
        func[0] = REL::Relocation<uintptr_t>{ RE::VTABLE_ValueModifierEffect[0] }.write_vfunc(10, thunk<0>);
        func[1] = REL::Relocation<uintptr_t>{ RE::VTABLE_DualValueModifierEffect[0] }.write_vfunc(10, thunk<1>);
        func[2] = REL::Relocation<uintptr_t>{ RE::VTABLE_AccumulatingValueModifierEffect[0] }.write_vfunc(10, thunk<2>);
        func[3] = REL::Relocation<uintptr_t>{ RE::VTABLE_PeakValueModifierEffect[0] }.write_vfunc(10, thunk<3>);
        func[4] = REL::Relocation<uintptr_t>{ RE::VTABLE_EnhanceWeaponEffect[0] }.write_vfunc(10, thunk<4>);

        logger::info("ValueEffectLoadGameHook complete...");
    }


    template <int I = 0>
    static void thunk(RE::ValueModifierEffect* a_this)
    {
        constexpr auto applied_effect_flag = RE::ActiveEffect::Flag(1 << 16);

        func[I](a_this);

        float alignment = a_this->magnitude >= 0 ? 1 : -1;

        //This hit even though it was false. Curious. 
        // The idea works, however it will definitely have issues
        if (a_this->flags.all(applied_effect_flag, RE::ActiveEffect::Flag::kRecovers) && 
            effect->baseEffect->data.flags.all(SettingFlag::kDetrimental) == true) &&
            (a_this->conditionStatus == RE::ActiveEffect::ConditionStatus::kTrue ||
            !a_this->flags.any(RE::ActiveEffect::Flag::kHasConditions)))//Has effects applied currently
            //HandleSpeedEffect(a_this, a_this->magnitude, I == 1, true);
            HandleSpeedEffect(a_this, a_this->effect->GetMagnitude() * alignment, I == 1, true);

        switch (a_this->actorValue)
        {
        case RE::ActorValue::kWeaponSpeedMult:
        case RE::ActorValue::kLeftWeaponSpeedMultiply:
            //Only allowed to proceed if it's recovers and the value is at or equal to 1.
            break;
        default:
            return;
        }

        static constexpr std::string_view names[]
        {
            "ValueMod",
            "DualMod",
            "AccumMod",
            "PeakMod",
            "Enhance"
        };
        

        logger::debug("LOAD {}: {}", names[I], a_this->magnitude);
    }




    static inline REL::Relocation<decltype(thunk<>)> func[5];
};


//write_call
struct ActorConstructorHook
{
    static void Patch()
    {
        REL::Relocation<uintptr_t> ctor_hook{ REL::RelocationID { 36195, 37174 }, 0x20 };//SE: 0x5CDBF0, AE: 604480, VR: ???

        auto& trampoline = SKSE::GetTrampoline();

        func = trampoline.write_call<5>(ctor_hook.address(), thunk);

        logger::info("ActorCtorHook complete...");
        //*/
    }

    static RE::Actor* thunk(RE::Actor* a_this)
    {
        
        //logger::debug("As bound {:X}, as magic {:X}", (uintptr_t)a_this, (uintptr_t)magic_item);

        //a_this->pad1C = 0;
        //a_this->GetActorRuntimeData().pad0EC = 0;
        GetActorTag(a_this, k_right) = 0;
        GetActorTag(a_this, k_left) = 0;

        return func(a_this);
    }

    static inline REL::Relocation<decltype(thunk)> func;
};


//VTABLE
struct Actor__FinishLoadGameHook
{
    static void Patch()
    {
        //*
        REL::Relocation<uintptr_t> PlayerCharacter__Actor_VTable{ RE::VTABLE_PlayerCharacter[0] };
        REL::Relocation<uintptr_t> Character__Actor_VTable{ RE::VTABLE_Character[0] };

        func[0] = PlayerCharacter__Actor_VTable.write_vfunc(0x11, thunk<0>);
        func[1] = Character__Actor_VTable.write_vfunc(0x11, thunk<1>);

        logger::info("Actor::FinishLoadGameHook complete...");
    }

    template <int I>
    static void thunk(RE::Character* a_this, RE::BGSLoadGameBuffer* a2)
    {
        func[I](a_this, a2);

        if (RE::AIProcess* process = a_this->GetActorRuntimeData().currentProcess; process){
            if (RE::CachedValues* cache = process->cachedValues; cache) {
                logger::trace("Resetting cache {:08X}", a_this->formID);
                ResetCache(cache, RE::ActorValue::kWeaponSpeedMult);
                ResetCache(cache, RE::ActorValue::kLeftWeaponSpeedMultiply);
            }
        }
    }



    static inline REL::Relocation<decltype(thunk<0>)> func[2];
    //static inline REL::Relocation<decltype(thunk)> func_;
};



//write_branch, rewrite
struct SetEffectivenessHook
{
    static void ScrambugsPatch()
    {

        std::ifstream scramble{ "Data/SKSE/Plugins/ScrambledBugs.json" };

        if (!scramble)
            return;

        logger::info("Scrambled bugs detected, reading settings...");

        //prefers scrambugs version over this fix if that fix in enabled
        nlohmann::json scrambugs_json = nlohmann::json::parse(scramble, nullptr, true, true);
        
        bool is_enabled = false;
        
        try
        {
            is_enabled = scrambugs_json["fixes"]["magicEffectFlags"].get<bool>();

            if (is_enabled) {
                patch_mult = false;
                logger::info("Scrambled bugs setting parsed using magicEffectFlags fix.");
            }
            else {
                logger::info("Scrambled Bugs magicEffectFlags setting disabled. Using modified version.");
            }
        }
        catch (nlohmann::json::exception& error)
        {
            logger::info("Scrambled bugs does not include 'fixes/magicEffectFlags'. Using modified version.");
        }
    }


    static void Patch()
    {
        //Use variantID at some point pls.
        //SE: 0x540360, AE: NA(inlined), VR: ???
        //7B aint the real hook, nor EA
        auto inner_hook_addr = REL::RelocationID(33320, 0, 0).address();
        
        //SE: 0x53DEB0, AE: 0x55EEA0, VR: ???
        auto outer_hook_addr = REL::RelocationID(33277, 34052, 0).address();
        
        //SE: 0x554700, AE: 0x5771B0, VR: ???//0x4A3/0x656
        auto wrap_hook_addr = REL::RelocationID(33763, 34547, 0).address() + REL::VariantOffset(0x4A3, 0x656, 0).offset();


        auto& trampoline = SKSE::GetTrampoline();


        
        /*
        auto hook_offset = REL::VariantOffset(0x76, 0xE5, 0x0).offset();
        auto return_offset = REL::VariantOffset(0xA7, 0x112, 0x0).offset();

        logger::debug("{:X} + {:X}/{:X}", hook_addr, hook_offset, return_offset);

        struct Code : Xbyak::CodeGenerator
        {
            Code(uintptr_t ret_addr, uintptr_t func)
            {
                mov(rax, func);

                //AE/SE versions remain the same
                switch(REL::Module::GetRuntime())
                {
                case REL::Module::Runtime::AE:
                    mov(rcx, rbx);
                    //movss(xmm2, xmm6);
                    break;

                case REL::Module::Runtime::SE:
                    mov(rcx, rdx);
                    //movss(xmm2, xmm1);
                    break;

                case REL::Module::Runtime::VR:
                    MessageBox(NULL, L"This fix does not currently support VR.", L"Invalid Module Detected", MB_OK);
                    logger::critical("This fix does not currently support VR. Terminating program.");
                    throw nullptr;

                default:
                    MessageBox(NULL, L"Unknown version of skyrim detected.", L"Invalid Module Detected", MB_OK);
                    logger::critical("Unknown version of skyrim detected. Terminating program.");
                    throw nullptr;
                }
                call(rax);

                mov(rax, ret_addr);
                jmp(rax);
            }
        } static code{ hook_addr + return_offset, (uintptr_t)thunk };



        trampoline.write_branch<5>(hook_addr + hook_offset, code.getCode());
        /*/
        
        if (inner_hook_addr)
            trampoline.write_branch<5>(inner_hook_addr, inner_thunk);

        trampoline.write_branch<5>(outer_hook_addr, outer_thunk);

        trampoline.write_call<5>(wrap_hook_addr, wrap_thunk);

        //*/
        
        //auto size = code.getSize();
        //auto result = trampoline.allocate(size);
        //std::memcpy(result, code.getCode(), size);
        //trampoline.write_branch<6>(hook_addr + hook_offset, (uintptr_t)result);

        logger::info("SetEffectiveness Hook complete...");
        ScrambugsPatch();
    }


    static bool ShouldAdjustEffects(const RE::MagicItem* a_this)
    {
        switch (a_this->GetSpellType())
        {
        case RE::MagicSystem::SpellType::kDisease:
        case RE::MagicSystem::SpellType::kAbility:
        case RE::MagicSystem::SpellType::kIngredient:
        case RE::MagicSystem::SpellType::kAddiction:
        {
            return false;
        }
        case RE::MagicSystem::SpellType::kEnchantment:
        {
            return a_this->GetCastingType() != RE::MagicSystem::CastingType::kConstantEffect;
        }
        default:
        {
            return true;
        }
        }
    }


    static void func(RE::ActiveEffect* a_this, float effectiveness)
    {
        float mag_comp = fabs(magnitudeComparison.GetValue());

        float next_increment = nextafter(mag_comp, INFINITY) - mag_comp;

        logger::debug("next increment value: {}", next_increment);

        float polarity = a_this->magnitude < 0 ? -1 : 1;

        a_this->magnitude = fmax(fabs(a_this->magnitude) * effectiveness, next_increment) * polarity;
        //a_this->magnitude = fabs(a_this->magnitude) * mult * polarity;

    }


    static void inner_thunk(RE::ActiveEffect* a_this, float effectiveness)
    {
        /*
        RE::Effect* effect = a_this->effect;
        RE::EffectSetting* effect_setting = effect->baseEffect;
        uint32_t setting_flags = effect->baseEffect->data.flags.underlying();
        
        //0x600
        constexpr auto no_mag_dur = stl::enumeration(SettingFlag::kNoMagnitude, SettingFlag::kNoDuration).underlying();
        {

            uint32_t effect_flags = a_this->flags.underlying();
            uint32_t spec_flags = effect_flags >> 12;

            logger::info("effective {} for {:08X}", effectiveness, a_this->effect->baseEffect->formID);
            
            //these correctly seem to be something related to duration and magnitude.
            //Although, which does which I'm unsure, but I know these likely ignore the settings somewhat
            auto ret_addr = (uintptr_t)_ReturnAddress();
            logger::info("checks; mag: {}, dur: {}, returns at {:X}", (effect_flags & 0x1000), (spec_flags & 0x1), ret_addr);
        }
        if (setting_flags != no_mag_dur && effectiveness != 1.f && effectiveness >= 0.f)
        {
            
            uint32_t effect_flags = a_this->flags.underlying();
            uint32_t spec_flags = effect_flags >> 12;
            //0x400
            if ((setting_flags & (uint32_t)SettingFlag::kNoMagnitude) == 0
                //"0x1000 << 12" and "0x400000" is an unknown flag.
                && (((effect_flags & 0x1000) == 0) || (setting_flags & (uint32_t)SettingFlag::kPowerAffectsDuration) == 0)
                //"0x1 << 12" and "0x200000" are both unknown flags currently. correction, 0x1 is a check for dual casting.
                //tired and tipsy, cant figure out how to pry out, just gonna leave it in and invalidate.
                || ((a_this->duration *= effectiveness || true) && (spec_flags & 1) != 0) && ((effect_flags & (uint32_t)SettingFlag::kPowerAffectsMagnitude) != 0))
            {
                return func(a_this, effectiveness);
            }
        }  
        return;
        //*/
        if constexpr (false)
        {
            RE::Effect* effect = a_this->effect;
            RE::EffectSetting* effect_setting = effect->baseEffect;
            uint32_t setting_flags = effect->baseEffect->data.flags.underlying();


            uint32_t effect_flags = a_this->flags.underlying();
            uint32_t spec_flags = effect_flags >> 12;

            logger::info("effective {} for {:08X}", effectiveness, a_this->effect->baseEffect->formID);

            logger::info("checks; ig?: {}, dual cast: {}", (effect_flags & 0x1000), (spec_flags & 0x1));
        }

        //Inner func, might have to implement upper changes in as well.
        auto effectSettingFlags = a_this->effect->baseEffect->data.flags;


        if (effectiveness == 1.f || effectiveness < 0.f)
        {
            return;
        }

        //Additional notations for this fix, in vanilla it would seem that resistances do not seem to account for influence duration.
        // Reflect these changes.

        //Mitigation mustnt be 1, which means it is mitigating, and scrambugs doesn't take priority
        if (mitigation == 0 || effectSettingFlags.none(SettingFlag::kNoDuration) && effectSettingFlags.all(SettingFlag::kPowerAffectsDuration))
        {
            a_this->duration *= effectiveness;
        }

        //Instead of using this, I will be using a different flag. Probably a padding value to tell me if I should use this or not. if the padding
        // is true, it force goes. if not, then it will evaluate
        // pad86 makes a good one. Additionally, if kernals mod is enabled with the json saying it wishes to use the other one, 
        // it will use that one.
        if (mitigation == 1 || effectSettingFlags.none(SettingFlag::kNoMagnitude) && effectSettingFlags.all(SettingFlag::kPowerAffectsMagnitude))
        {
            return func(a_this, effectiveness);
        }
    }

    static void outer_thunk(RE::ActiveEffect* a_this, float effectiveness, bool req_hostile)
    {
        RE::MagicItem* magic_item = a_this->spell;

        /*
        
        if (magic_item)
        {
            static const long v7 = 0x512;//1298;

            RE::MagicSystem::SpellType magic_type = magic_item->GetSpellType();

            if (auto m_type = static_cast<long>(magic_type); m_type > 0xA || !_bittest(&v7, m_type))
            {
                RE::MagicSystem::CastingType cast_type = magic_item->GetCastingType();

                if ((magic_type != RE::MagicSystem::SpellType::kEnchantment || 
                    cast_type != RE::MagicSystem::CastingType::kConstantEffect)
                    && (!a3 || a_this->effect->IsHostile()))
                {
                    return inner_thunk(a_this, a2);
                }
            }
        }
        //*/
        //auto ret_addr = (uintptr_t)_ReturnAddress();
        //auto  return_override = REL::RelocationID(33393, 34175, 0).address() + 0x15;//SE: 0x543D70, AE: 0x565B60
        //logger::info("pre-checks; expected return? {}", return_override == ret_addr);

        
        if (ShouldAdjustEffects(magic_item) == false)
        {
            return;
        }

        if (req_hostile && !a_this->effect->IsHostile())
        {
            return;
        }

        return inner_thunk(a_this, effectiveness);
    }



    static void wrap_thunk(RE::ActiveEffect* a_this, float effectiveness, bool req_hostile)
    {
        if (patch_mult)
            mitigation = 1;

        outer_thunk(a_this, effectiveness, req_hostile);

        if (patch_mult)
            mitigation = 0;
    }


    //So right here, I'll make thunk_call. It will wrap the outer_thunk, and adjust the flags between calls.

    static uintptr_t get_thunk()
    {
        switch (REL::Module::GetRuntime())
        {
        case REL::Module::Runtime::AE:
            return reinterpret_cast<uintptr_t>(outer_thunk);

        case REL::Module::Runtime::SE:
            return reinterpret_cast<uintptr_t>(inner_thunk);

        case REL::Module::Runtime::VR:
            MessageBox(NULL, L"This fix does not currently support VR.", L"Invalid Module Detected", MB_OK);
            logger::critical("This fix does not currently support VR. Terminating program.");
            throw nullptr;

        default:
            MessageBox(NULL, L"Unknown version of skyrim detected.", L"Invalid Module Detected", MB_OK);
            logger::critical("Unknown version of skyrim detected. Terminating program.");
            throw nullptr;
        }
    }

    static inline bool patch_mult = true;
    static inline thread_local int8_t mitigation = -1;

    //static inline REL::Relocation<decltype(thunk)> func;
};


constexpr std::string_view installedString = "CASP_InstallState";
constexpr std::string_view papyrusAPIString = "CASP_PapyrusAPI";



int32_t GetVerisonNumber(RE::StaticFunctionTag* = nullptr)
{
    return 1;
}





bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
{
    a_vm->RegisterFunction("VersionNumber", papyrusAPIString, GetVerisonNumber);

    a_vm->RegisterFunction("GetEffectiveWeaponSpeed", papyrusAPIString, GetEffectiveSpeedFromActor);

    logger::info("PapyrusAPI registered.");

    return true;
};

//write_branch
struct Condition_HasKeywordHook
{
    static void Patch()
    {
        //SE: 0x2DDA40, AE: 0x2F3C80, VR: ???
        auto hook_addr = REL::RelocationID(21187, 21644, 0).address();
        auto return_addr = hook_addr + 0x6;
        //*
        struct Code : Xbyak::CodeGenerator
        {
            Code(uintptr_t ret_addr)
            {
                //AE/SE versions remain the same
                push(rbx);
                sub(rsp, 0x20);

                mov(rax, ret_addr);
                jmp(rax);
            }
        } static code{ return_addr };

        auto& trampoline = SKSE::GetTrampoline();

        auto placed_call = IsCallOrJump(hook_addr) > 0;

        auto place_query = trampoline.write_branch<5>(hook_addr, (uintptr_t)thunk);

        if (!placed_call)
            func = (uintptr_t)code.getCode();
        else
            func = place_query;

        logger::info("Condition_HasKeywordHook complete...");
        //*/
    }

    static bool thunk(RE::TESObjectREFR* a_this, RE::BGSKeyword* a2, void* a3, double* a4)
    {
        if (stricmp(a2->formEditorID.c_str(), installedString.data()) == 0)
        {
            //Later, this value can also mean didn't install right if it's -1
            // This also might mean version so if you want to check if it's installed, do 
            // != 0
            *a4 = GetVerisonNumber();

            const auto log = RE::ConsoleLog::GetSingleton();
            
            if (log->IsConsoleMode() == true)
                log->Print("CASP Installed. Version >> %0.2f", *a4);

            return true;
        }else
        {
            return func(a_this, a2, a3, a4);
        }
    }

    static inline REL::Relocation<decltype(thunk)> func;
};


//*/
void AddSettings()
{
    {
        auto* collection = RE::GameSettingCollection::GetSingleton();

        collection->InsertSetting(minSpeed);
        collection->InsertSetting(capSpeed);
        collection->InsertSetting(speedTaper);
        collection->InsertSetting(maxSpeed);
    }
    {
        auto* collection = RE::INISettingCollection::GetSingleton();
        //Make this an INI setting.
        collection->InsertSetting(magnitudeComparison);
    }
}

void InitializeMessaging() {
    //Make a function in AVG so that one can get the effective speed mult(which is the speed mult that you'd see when swings happen).
    
    if (!GetMessagingInterface()->RegisterListener([](MessagingInterface::Message* message) {
        switch (message->type) {
        case MessagingInterface::kPostPostLoad:
            SetEffectivenessHook::Patch();
            break;
        }
        })) {
        stl::report_and_fail("Unable to register message listener.");
    }
}


SKSEPluginLoad(const LoadInterface* skse) {
    Init(skse);

    InitializeLogging();
    InitializeMessaging();
    auto& trampoline = SKSE::GetTrampoline();

    //SKSE::AllocTrampoline(98);//Not implementing the hook that requires this
    SKSE::AllocTrampoline(126);//+14 for 1 unimplemented hook.
    
    WeaponSpeedMultHook::Patch();
    CreateActorValueInfoHook::Patch();
    ValueEffectStartHook::Patch();
    ValueEffectFinishHook::Patch();
    ValueEffectLoadGameHook::Patch();
    GetActorValueHook::Patch();
    GetActorValueModifierHook::Patch();
    ActorConstructorHook::Patch();
    Actor__FinishLoadGameHook::Patch();
    Condition_HasKeywordHook::Patch();
    

    auto papyrus = SKSE::GetPapyrusInterface();
    if (!papyrus->Register(RegisterFuncs)) {
        return false;
    }

    //SetEffectivenessHook::Patch();//Hooking post load for compatibility with scrambled bugs
    
    AddSettings();
   
    return true;

}
