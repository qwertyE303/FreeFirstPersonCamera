#include <Windows.h>

#include <BML/BMLAll.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef FFPC_VERSION
#define FFPC_VERSION "0.1.0"
#endif

// BML 0.3.40 has no MOD_EXPORT macro (BML+ exports it from BML/Export.h).
#ifndef MOD_EXPORT
#define MOD_EXPORT extern "C" __declspec(dllexport)
#endif

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Pitch limit used when 360 mode is off (85 degrees).
constexpr float kPitchLimit = 1.48352986f;

// First-person near plane.  The game camera uses 3.0, which clips nearby world
// geometry far too aggressively.  The camera sits at the ball centre (radius
// 2.0), so 1.0 keeps the surrounding world visible without cutting the ball.
constexpr float kNearPlane = 1.0f;

inline VxVector Cross(const VxVector &a, const VxVector &b) {
    return VxVector(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

inline float WrapRadians(float angle) {
    while (angle > kPi) angle -= 2.0f * kPi;
    while (angle <= -kPi) angle += 2.0f * kPi;
    return angle;
}

inline float WrapDegrees(float angle) {
    while (angle > 180.0f) angle -= 360.0f;
    while (angle <= -180.0f) angle += 360.0f;
    return angle;
}

} // namespace

// First-person camera for Ballance / BML+.
//
// Force handling:
//   Ball Navigation has four movement SetPhysicsForce behaviors whose Direction
//   inputs are (1,0,0) / (-1,0,0) / (0,0,-1) / (0,0,1).  Their referential is
//   replaced by our own entity, whose world matrix is built with
//   row0 = right, row1 = up, row2 = forward.
//
//   The force is frozen until the level re-runs those behaviors, which it does
//   when its own camera is rotated.  The mouse therefore also drives the level
//   camera (see DriveGameCamera), but only as a trigger: the direction always
//   comes from our referential.  Nothing is duplicated and the normal key
//   release still clears the game's own force.
//
//   With 360 mode off the referential is horizontal (yaw only), matching the
//   game's own pure yaw reference, so looking up or down never pushes the ball
//   into the air.  With 360 mode on the full view basis (pitch and roll
//   included) is used.
//
// Keyboard:
//   Instead of writing the key code into the parameter the game writes, the four
//   Key Event inputs are disconnected from the game's parameter and pointed at
//   our own parameters (SetDirectSource).  The level script re-writes its own
//   parameters a few frames after StartLevel, but that cannot reach our inputs
//   any more, so there is no race and no polling.
//
// Initial view:
//   The level script places InGameCam only when the camera navigation becomes
//   active, so at StartLevel the camera still carries the previous level's pose.
//   The view follows InGameCam from OnCamNavActive and is frozen at
//   OnBallNavActive, the frame on which the game hands the ball to the player.
//
// Cursor:
//   With ShowCursor off the game cursor is hidden and the OS cursor is pinned
//   to the centre of the client area every frame, so mouse look can never walk
//   the pointer out of the window.  Pausing (ESC), losing focus and leaving the
//   level release the pin and restore the cursor.
class FreeFirstPersonCamera final : public IMod {
  public:
    explicit FreeFirstPersonCamera(IBML *bml) : IMod(bml), m_BmlPtr(bml) {}

    const char *GetID() override { return "FreeFirstPersonCamera"; }
    const char *GetVersion() override { return FFPC_VERSION; }
    const char *GetName() override { return "FreeFirstPersonCamera"; }
    const char *GetAuthor() override { return "Entity_303-E3"; }
    const char *GetDescription() override {
        return "First-person view with free camera rotation. Based on FirstPersonCamera and FreeViewRotation by BallanceBug";
    }
    DECLARE_BML_VERSION;

    void OnLoad() override {
        IConfig *cfg = GetConfig();
        if (cfg) {
            cfg->SetCategoryComment("Main", "Basic switches and input.");

            m_EnabledProp = cfg->GetProperty("Main", "Enabled");
            m_EnabledProp->SetDefaultBoolean(true);
            m_EnabledProp->SetComment("Global switch.");

            m_ToggleKeyProp = cfg->GetProperty("Main", "ToggleKey");
            m_ToggleKeyProp->SetDefaultKey(CKKEY_F);
            m_ToggleKeyProp->SetComment("Toggle between first-person and third-person temporarily in game.");

            m_WasdModeProp = cfg->GetProperty("Main", "WASDMode");
            m_WasdModeProp->SetDefaultBoolean(true);
            m_WasdModeProp->SetComment(
                    "Use W/A/S/D instead of default keys while first-person is active.");

            m_ShowCursorProp = cfg->GetProperty("Main", "ShowCursor");
            m_ShowCursorProp->SetDefaultBoolean(true);
            m_ShowCursorProp->SetComment(
                    "Show the cursor while first-person is active.");

            cfg->SetCategoryComment("Camera", "Camera behaviour and mouse look.");

            m_SensitivityProp = cfg->GetProperty("Camera", "Sensitivity");
            m_SensitivityProp->SetDefaultFloat(4.0f);
            m_SensitivityProp->SetComment("Mouse sensitivity.");

            m_InvertXProp = cfg->GetProperty("Camera", "InvertX");
            m_InvertXProp->SetDefaultBoolean(false);
            m_InvertXProp->SetComment("Invert horizontal rotation.");

            m_InvertYProp = cfg->GetProperty("Camera", "InvertY");
            m_InvertYProp->SetDefaultBoolean(false);
            m_InvertYProp->SetComment("Invert vertical rotation.");

            m_360ModeProp = cfg->GetProperty("Camera", "360Mode");
            m_360ModeProp->SetDefaultBoolean(false);
            m_360ModeProp->SetComment(
                    "When on, the pitch is unlimited, and left/right always turns around "
                    "the view's own up axis, and the force follows the view's own axis.  "
                    "When off, pitch is limited and the force stays horizontal.");

            cfg->SetCategoryComment("Debug", "Diagnostics.");

            m_DebugProp = cfg->GetProperty("Debug", "DebugLog");
            m_DebugProp->SetDefaultBoolean(false);
            m_DebugProp->SetComment("Write ball/camera diagnostics to ModLoader.log once per second.");
        }

        ReloadConfig();
        // Launching the game always starts with the temporary switch on; later
        // level loads / respawns keep whatever F last selected.
        m_TempOn = true;
        GetLogger()->Info("FreeFirstPersonCamera %s loaded; toggle key is 0x%X, master=%d.",
                          FFPC_VERSION, (unsigned)m_ToggleKey, m_Enabled ? 1 : 0);
    }

    void OnUnload() override {
        Deactivate("unload");
        DestroyRig();
        RestoreHooks();
        RestoreGameCameraDuration();
        m_Paused = false;
        ReleaseCursorLock(false);
    }

    void OnModifyConfig(const char *category, const char *key, IProperty *prop) override {
        const bool wasEnabled = m_Enabled;
        const bool wasTemp = m_TempOn;
        const bool enabledKey =
                (key && std::strcmp(key, "Enabled") == 0) || prop == m_EnabledProp;
        ReloadConfig();
        // Only the master switch in the settings may change the on/off state.
        // F is a separate temporary switch and changing 360Mode / ShowCursor /
        // ... must leave it alone.
        if (!enabledKey) {
            m_Enabled = wasEnabled;
            m_TempOn = wasTemp;
        } else if (m_Enabled) {
            // Re-enabling the mod always comes back in first person.
            m_TempOn = true;
        } else {
            // Master off: drop the temporary state and hand the game its own
            // camera back.  Do not write InGameCam here: the level's camera
            // navigation owns it and restores the default third-person view by
            // itself when the game resumes.
            m_TempOn = false;
            m_ViewLocked = false;
            m_ViewFollowing = true;
            if (m_Active) {
                Deactivate("disabled by config");
            }
        }
        if (m_Active) {
            UpdateKeyValues();
            if (!m_360Mode) {
                ClampViewToPitchLimit();
            }
        }
        UpdateCursorLock();
    }

    void OnPostLoadLevel() override {
        m_CameraId = 0;
        m_GameRefId = 0;
        m_ForceRefId = 0;
        m_Keys.clear();
        m_KeysReady = false;
        m_RefLinks.clear();
        m_RefReady = false;
        m_Active = false;
        m_ViewLocked = false;
        m_ViewFollowing = false;
        m_RespawnPending = false;
        m_BallActive = false;
        m_KeyFixes = 0;
        m_LoggedKeys = false;
        m_LoggedLayout = false;
        m_HasDefaultDuration = false;
        m_DurationOverridden = false;
        m_HasDefaultCameraRotation = false;
        m_HasBallPos = false;
        m_Paused = false;
        FindGameCameraBBs();
        ReleaseCursorLock(false);
        GetLogger()->Info("[FFPC] PostLoadLevel");
    }

    void OnStartLevel() override {
        m_ViewLocked = false;
        m_ViewFollowing = false;
        m_RespawnPending = true;
        m_BallActive = false;
        m_Paused = false;
        m_ViewDistance = 0.0f;
        CacheWidestViewDistance();
        ReleaseCursorLock(false);
        GetLogger()->Info("[FFPC] StartLevel (viewdist saved=%.0f)", m_ViewDistance);
        LogPose("StartLevel");
    }

    void OnCamNavActive() override {
        m_ViewFollowing = true;
        m_RespawnPending = true;
        GetLogger()->Info("[FFPC] CamNavActive (following game camera)");
        LogPose("CamNavActive");
        // The camera is already anchored at the level / checkpoint pose here, so
        // the view is re-initialised immediately instead of waiting for the ball
        // to be handed back (which happens several seconds later).
        if (m_RespawnPending && LockViewFromCamera("CamNavActive")) {
            m_RespawnPending = false;
        }
    }

    void OnBallNavActive() override {
        m_BallActive = true;
        GetLogger()->Info("[FFPC] BallNavActive");
        LogPose("BallNavActive");
        if (m_RespawnPending && LockViewFromCamera("BallNavActive")) {
            m_RespawnPending = false;
        }
    }

    void OnPreResetLevel() override { GetLogger()->Info("[FFPC] PreResetLevel"); }

    void OnPostResetLevel() override {
        m_ViewLocked = false;
        m_ViewFollowing = false;
        m_RespawnPending = true;
        m_BallActive = false;
        m_Paused = false;
        ReleaseCursorLock(false);
        GetLogger()->Info("[FFPC] PostResetLevel");
        LogPose("PostResetLevel");
    }

    void OnPauseLevel() override {
        m_Paused = true;
        UpdateCursorLock();
        GetLogger()->Info("[FFPC] PauseLevel (keeping first-person, releasing cursor)");
    }

    // Death does not reload the level: the game only fires BallOff and later
    // restarts camera / ball navigation at the checkpoint.  Drop the view lock so
    // the usual CamNavActive -> BallNavActive sequence re-initialises it.
    void OnBallOff() override { ResetViewForRespawn("BallOff"); }

    void OnDead() override { ResetViewForRespawn("Dead"); }

    void OnUnpauseLevel() override {
        m_Paused = false;
        GetLogger()->Info("[FFPC] UnpauseLevel");
        LogPose("UnpauseLevel");
        UpdateCursorLock();
    }

    void OnPreExitLevel() override {
        Deactivate("exit level");
        DestroyRig();
        RestoreHooks();
        m_Paused = false;
        m_BallActive = false;
        ReleaseCursorLock(false);
    }

    void OnPostExitLevel() override {
        DestroyRig();
        RestoreHooks();
        m_ViewLocked = false;
        m_ViewFollowing = false;
        m_RespawnPending = false;
        m_BallActive = false;
        m_Paused = false;
        ReleaseCursorLock(false);
    }

    void OnPreNextLevel() override {
        Deactivate("next level");
        DestroyRig();
        RestoreHooks();
        m_Paused = false;
        m_BallActive = false;
        ReleaseCursorLock(false);
    }

    void OnProcess() override {
        ++m_Frame;

        InputHook *input = BML()->GetInputManager();
        VxVector mouseDelta(0.0f, 0.0f, 0.0f);
        if (input) {
            input->GetMouseRelativePosition(mouseDelta);
        }

        if (input && m_ToggleKey != 0 && m_Enabled && BML()->IsPlaying() &&
            input->IsKeyPressed(m_ToggleKey)) {
            m_TempOn = !m_TempOn;
            if (m_TempOn) {
                BML()->SendIngameMessage("[FFPC] first-person ON");
            } else {
                Deactivate("toggled off");
                BML()->SendIngameMessage("[FFPC] first-person OFF");
            }
        }

        if (!m_Enabled) {
            if (m_Active) {
                Deactivate("disabled");
            }
            m_TempOn = false;
            UpdateCursorLock();
            return;
        }

        if (!m_TempOn) {
            if (m_Active) {
                Deactivate("temporary off");
            }
            UpdateCursorLock();
            return;
        }

        if (!BML()->IsPlaying() || BML()->IsPaused() || m_Paused) {
            if (m_Active) {
                KeepAttached();
            }
            UpdateCursorLock();
            return;
        }

        if (!m_Active && !Activate()) {
            UpdateCursorLock();
            return;
        }

        EnsureHooks();

        ReadMouseLook(mouseDelta);
        UpdateForceReference();
        DriveGameCamera(mouseDelta);
        FollowGameCamera();

        UpdateCamera();
        UpdateBallMotion();
        KeepAttached();
        UpdateCursorLock();

        if (m_Debug && (m_Frame % 60) == 0) {
            LogState();
        }
    }

  private:
    struct RefLink {
        CKParameterIn *input = nullptr;
        CKParameter *original = nullptr;
    };

    struct KeyLink {
        CKParameterIn *input = nullptr;
        CKParameter *original = nullptr;
        CKParameterLocal *own = nullptr;
        int originalCode = 0;
        int wasdCode = 0;
        int boundCode = 0;
        std::string name;
    };

    // ---------------------------------------------------------------- config
    void ReloadConfig() {
        m_Enabled = m_EnabledProp ? m_EnabledProp->GetBoolean() : true;
        m_Sensitivity = m_SensitivityProp ? m_SensitivityProp->GetFloat() : 4.0f;
        m_InvertX = m_InvertXProp ? m_InvertXProp->GetBoolean() : false;
        m_InvertY = m_InvertYProp ? m_InvertYProp->GetBoolean() : false;
        m_WasdMode = m_WasdModeProp ? m_WasdModeProp->GetBoolean() : true;
        m_ShowCursor = m_ShowCursorProp ? m_ShowCursorProp->GetBoolean() : true;
        m_360Mode = m_360ModeProp ? m_360ModeProp->GetBoolean() : false;
        m_ToggleKey = m_ToggleKeyProp ? m_ToggleKeyProp->GetKey() : CKKEY_F;
        m_Debug = m_DebugProp ? m_DebugProp->GetBoolean() : true;

        if (m_Sensitivity < 0.1f) m_Sensitivity = 0.1f;
        if (m_Sensitivity > 40.0f) m_Sensitivity = 40.0f;
    }

    // ------------------------------------------------------------- utilities
    IBML *BML() const { return m_BmlPtr; }
    CKContext *Ctx() const { return BML()->GetCKContext(); }
    CKRenderContext *Render() const { return BML()->GetRenderContext(); }

    CKTargetCamera *Camera() const {
        CKObject *o = Ctx() ? Ctx()->GetObject(m_CameraId) : nullptr;
        if (!o || !CKIsChildClassOf(o, CKCID_TARGETCAMERA)) return nullptr;
        return static_cast<CKTargetCamera *>(o);
    }

    // The camera that actually renders the game view before we attach our own.
    // On old BML, GetTargetCameraByName("InGameCam") can resolve a different
    // object than the active render camera; use the attached/viewpoint camera
    // first, and only fall back to the name lookup.
    CKCamera *ViewCamera() const {
        if (m_HasViewCamera) {
            CKObject *o = Ctx() ? Ctx()->GetObject(m_ViewCameraId) : nullptr;
            if (o && CKIsChildClassOf(o, CKCID_CAMERA)) {
                return static_cast<CKCamera *>(o);
            }
        }
        return BML()->GetTargetCameraByName("InGameCam");
    }

    // The active render camera is not exposed reliably by old BML, and the
    // named InGameCam can be a stale object.  Keep the widest far plane seen in
    // the scene so a level/visual mod's 1e9 survives the game resetting its own
    // camera later.
    void CacheWidestViewDistance() {
        CKContext *ctx = Ctx();
        if (!ctx) return;

        const XObjectPointerArray &cameras =
                ctx->GetObjectListByType(CKCID_CAMERA, TRUE);
        for (int i = 0; i < cameras.Size(); ++i) {
            CKObject *o = cameras[i];
            if (!o || !CKIsChildClassOf(o, CKCID_CAMERA)) continue;
            const float back = static_cast<CKCamera *>(o)->GetBackPlane();
            if (back > m_ViewDistance) m_ViewDistance = back;
        }
    }

    // The entity the level itself uses as the movement force referential.
    CK3dEntity *GameRef() const {
        CKObject *o = (Ctx() && m_GameRefId) ? Ctx()->GetObject(m_GameRefId) : nullptr;
        if (!o || !CKIsChildClassOf(o, CKCID_3DENTITY)) return nullptr;
        return static_cast<CK3dEntity *>(o);
    }

    // Our own force referential, built from the view every frame.
    CK3dEntity *ForceRef() const {
        CKObject *o = (Ctx() && m_ForceRefId) ? Ctx()->GetObject(m_ForceRefId) : nullptr;
        if (!o || !CKIsChildClassOf(o, CKCID_3DENTITY)) return nullptr;
        return static_cast<CK3dEntity *>(o);
    }

    // Reads the entity a 3D entity parameter points at.  Anything unexpected is
    // swallowed: a broken probe must not take the whole setup down with it.
    static CK_ID ResolveParameterEntity(CKParameter *param) {
        if (!param) return 0;
        try {
            if (param->GetGUID() != CKPGUID_3DENTITY) return 0;
            CKObject *o = param->GetValueObject();
            if (o && CKIsChildClassOf(o, CKCID_3DENTITY)) return o->GetID();
        } catch (...) {
        }
        return 0;
    }

    CK3dEntity *CurrentBall() const {
        CKDataArray *array = BML()->GetArrayByName("CurrentLevel");
        if (!array) return nullptr;
        CKObject *o = array->GetElementObject(0, 1);
        if (!o || !CKIsChildClassOf(o, CKCID_3DENTITY)) return nullptr;
        return static_cast<CK3dEntity *>(o);
    }

    CKBehavior *BallNavigation() const {
        CKBehavior *gameplay = BML()->GetScriptByName("Gameplay_Ingame");
        if (!gameplay) return nullptr;
        return ScriptHelper::FindFirstBB(gameplay, "Ball Navigation");
    }

    void ResetViewForRespawn(const char *reason) {
        GetLogger()->Info("[FFPC] %s: view unlocked, waiting for respawn camera", reason);
        LogPose(reason);
        m_ViewLocked = false;
        m_ViewFollowing = false;
        m_RespawnPending = true;
        m_BallActive = false;
    }

    // ------------------------------------------------------------- camera rig
    bool EnsureRig() {
        if (Camera()) return true;

        DestroyRig();

        CKContext *ctx = Ctx();
        if (!ctx || !ctx->GetCurrentLevel()) return false;

        auto *cam = static_cast<CKTargetCamera *>(
                ctx->CreateObject(CKCID_TARGETCAMERA, "FFPC_Camera", CK_OBJECTCREATION_DYNAMIC));
        if (!cam) {
            GetLogger()->Error("[FFPC] failed to create camera object");
            return false;
        }

        cam->SetFrontPlane(kNearPlane);
        cam->SetBackPlane(4000.0f);
        ctx->GetCurrentLevel()->AddObject(cam);

        m_CameraId = cam->GetID();
        GetLogger()->Info("[FFPC] created camera id=%u", (unsigned)m_CameraId);
        return true;
    }

    void DestroyRig() {
        CKContext *ctx = Ctx();
        if (!ctx) return;
        if (m_Active) {
            Deactivate("destroy rig");
        }
        if (CKObject *o = ctx->GetObject(m_CameraId)) ctx->DestroyObject(o);
        m_CameraId = 0;
    }

    // ------------------------------------------------------ movement force
    // The four movement forces are the level's own behaviors: their Direction
    // inputs are the fixed local vectors (0,0,1) / (0,0,-1) / (-1,0,0) / (1,0,0),
    // transformed by the force referential.  They are left untouched; only the
    // referential is pointed at our own entity, whose world matrix is rebuilt
    // from the view every frame (rows: right, up, forward).  That is what keeps
    // left and right matching the screen at any pitch, roll included.
    //
    // The force is only re-evaluated when the level re-runs those behaviors,
    // which it does when its own camera is rotated - so the camera is driven as
    // well (see DriveGameCamera) and no key re-press is needed.
    bool SetupForceReference() {
        if (m_RefReady) return true;

        CKContext *ctx = Ctx();
        if (!ctx || !ctx->GetCurrentLevel()) return false;

        CKBehavior *ballNav = BallNavigation();
        if (!ballNav) return false;
        if (!CurrentBall()) return false;

        if (!ForceRef()) {
            auto *ref = static_cast<CK3dEntity *>(
                    ctx->CreateObject(CKCID_3DENTITY, "FFPC_ForceRef",
                                      CK_OBJECTCREATION_DYNAMIC));
            if (!ref) {
                GetLogger()->Error("[FFPC] failed to create the force referential");
                return false;
            }
            ctx->GetCurrentLevel()->AddObject(ref);
            m_ForceRefId = ref->GetID();
        }

        if (!m_RefParam) {
            m_RefParam = ScriptHelper::CreateParamObject(ballNav, "FFPC_RefParam",
                                                         CKPGUID_3DENTITY, ForceRef());
            if (!m_RefParam) {
                GetLogger()->Error("[FFPC] failed to create the referential parameter");
                return false;
            }
        }

        m_RefLinks.clear();
        for (int i = 0; i < ballNav->GetSubBehaviorCount(); ++i) {
            CKBehavior *behavior = ballNav->GetSubBehavior(i);
            if (!behavior || !behavior->GetName()) continue;
            if (std::strcmp(behavior->GetName(), "SetPhysicsForce") != 0) continue;

            CKParameterIn *refIn = behavior->GetInputParameter(3);
            if (!refIn) continue;

            RefLink link;
            link.input = refIn;
            link.original = refIn->GetDirectSource();
            if (m_GameRefId == 0) {
                m_GameRefId = ResolveParameterEntity(link.original);
            }
            refIn->SetDirectSource(m_RefParam);
            m_RefLinks.push_back(link);
        }

        m_RefReady = !m_RefLinks.empty();
        GetLogger()->Info("[FFPC] force referential: %d movement force inputs re-pointed "
                          "(level referential id=%u)",
                          (int)m_RefLinks.size(), (unsigned)m_GameRefId);
        if (!m_LoggedLayout) {
            m_LoggedLayout = true;
            LogForceLayout(ballNav);
        }
        return m_RefReady;
    }

    // Diagnostic: what the force behaviors actually are.  Every direct sub
    // behavior of Ball Navigation is listed, then each SetPhysicsForce with its
    // local direction, magnitude and the behavior that activates it.
    void LogForceLayout(CKBehavior *ballNav) {
        const int count = ballNav->GetSubBehaviorCount();
        GetLogger()->Info("[FFPC] Ball Navigation direct sub behaviors: %d", count);
        for (int i = 0; i < count; ++i) {
            CKBehavior *behavior = ballNav->GetSubBehavior(i);
            if (!behavior) continue;
            GetLogger()->Info("[FFPC]   [%d] '%s' type=%d", i,
                              behavior->GetName() ? behavior->GetName() : "?",
                              (int)behavior->GetType());
        }
        for (int i = 0; i < count; ++i) {
            CKBehavior *behavior = ballNav->GetSubBehavior(i);
            if (!behavior || !behavior->GetName()) continue;
            if (std::strcmp(behavior->GetName(), "SetPhysicsForce") != 0) continue;
            LogForceDetails(ballNav, behavior, i);
        }

        // Which keys the level listens to.  The four we remapped are reported
        // with their original scan code; anything left over is a key the mod
        // never touches, so its force is only ever triggered by the player.
        m_ExtraKeys.clear();
        for (int i = 0; i < count; ++i) {
            CKBehavior *behavior = ballNav->GetSubBehavior(i);
            if (!behavior || !behavior->GetName()) continue;
            if (std::strcmp(behavior->GetName(), "Key Event") != 0) continue;

            CKParameterIn *input = behavior->GetInputParameter(0);
            if (!input) continue;
            CKParameter *src = input->GetRealSource();
            if (!src) continue;
            int code = 0;
            if (src->GetValue(&code) != CK_OK) continue;

            const KeyLink *bound = nullptr;
            for (auto &key : m_Keys) {
                if (key.input == input) {
                    bound = &key;
                    break;
                }
            }
            if (bound) {
                GetLogger()->Info("[FFPC] keyevent[%d] code=%d remapped to %d ('%s')", i,
                                  bound->originalCode, bound->boundCode, bound->name.c_str());
            } else {
                GetLogger()->Info("[FFPC] keyevent[%d] code=%d untouched by the mod", i, code);
                m_ExtraKeys.push_back(code);
            }
        }
    }

    void LogForceDetails(CKBehavior *ballNav, CKBehavior *behavior, int index) {
        VxVector dir(0.0f, 0.0f, 0.0f);
        VxVector pos(0.0f, 0.0f, 0.0f);
        float force = 0.0f;

        if (CKParameterIn *in = behavior->GetInputParameter(2)) {
            if (CKParameter *src = in->GetRealSource()) src->GetValue(&dir);
        }
        if (CKParameterIn *in = behavior->GetInputParameter(1)) {
            if (in->GetGUID() == CKPGUID_VECTOR) behavior->GetInputParameterValue(1, &pos);
        }
        if (CKParameterIn *in = behavior->GetInputParameter(4)) {
            if (in->GetGUID() == CKPGUID_FLOAT) behavior->GetInputParameterValue(4, &force);
        }

        char activators[192] = {0};
        for (int k = 0; k < ballNav->GetSubBehaviorLinkCount(); ++k) {
            CKBehaviorLink *link = ballNav->GetSubBehaviorLink(k);
            if (!link || !link->GetInBehaviorIO()) continue;
            if (link->GetInBehaviorIO()->GetOwner() != behavior) continue;

            CKBehaviorIO *outIO = link->GetOutBehaviorIO();
            CKBehavior *from = outIO ? outIO->GetOwner() : nullptr;
            const int inIndex = behavior->GetInputPosition(link->GetInBehaviorIO());
            char piece[96];
            std::snprintf(piece, sizeof(piece), "%s@in%d",
                          from && from->GetName() ? from->GetName() : "?", inIndex);
            if (activators[0]) strncat_s(activators, sizeof(activators), "|", 1);
            strncat_s(activators, sizeof(activators), piece, _TRUNCATE);
        }

        GetLogger()->Info("[FFPC] force[%d] dir=(%.2f,%.2f,%.2f) force=%.3f pos=(%.2f,%.2f,%.2f) "
                          "activated by '%s'",
                          index, dir.x, dir.y, dir.z, force, pos.x, pos.y, pos.z,
                          activators[0] ? activators : "<none>");
    }

    void ReassertForceReference() {
        for (auto &link : m_RefLinks) {
            if (!link.input) continue;
            if (link.input->GetDirectSource() == m_RefParam) continue;
            link.input->SetDirectSource(m_RefParam);
        }
    }

    // The basis the level's movement forces are expressed in.  Non-360 mode is
    // flattened to yaw so the force stays horizontal; 360 mode uses the full
    // view basis (pitch and roll included).
    void ForceBasis(VxVector &forward, VxVector &up) const {
        forward = Forward();
        up = Up();
        if (!m_360Mode) {
            forward.y = 0.0f;
            if (forward.Magnitude() < 1e-4f) {
                forward.Set(0.0f, 0.0f, 1.0f);
            }
            forward.Normalize();
            up.Set(0.0f, 1.0f, 0.0f);
        }
    }

    // Rows are right / up / forward, so the local force vectors map to the view.
    void UpdateForceReference() {
        CK3dEntity *ref = ForceRef();
        CK3dEntity *ball = CurrentBall();
        if (!ref || !ball) return;

        VxVector ballPos(0.0f, 0.0f, 0.0f);
        ball->GetPosition(&ballPos);

        VxVector forward, up;
        ForceBasis(forward, up);

        VxVector right = Cross(up, forward);
        right.Normalize();

        VxMatrix matrix;
        matrix[0] = VxVector4(right.x, right.y, right.z, 0.0f);
        matrix[1] = VxVector4(up.x, up.y, up.z, 0.0f);
        matrix[2] = VxVector4(forward.x, forward.y, forward.z, 0.0f);
        matrix[3] = VxVector4(ballPos.x, ballPos.y, ballPos.z, 1.0f);
        ref->SetWorldMatrix(matrix);
    }

    void RestoreForceReference() {
        for (auto &link : m_RefLinks) {
            if (link.input && link.original) {
                link.input->SetDirectSource(link.original);
            }
        }
        m_RefLinks.clear();

        CKContext *ctx = Ctx();
        if (ctx && m_RefParam) {
            ctx->DestroyObject(m_RefParam);
            m_RefParam = nullptr;
        }
        if (ctx && m_ForceRefId) {
            if (CKObject *o = ctx->GetObject(m_ForceRefId)) ctx->DestroyObject(o);
            m_ForceRefId = 0;
        }
        m_RefReady = false;
    }

    // ------------------------------------------------------------- key binding
    static int WasdForKeyCode(int code) {
        switch (code) {
            case 200: return (int)CKKEY_W;
            case 208: return (int)CKKEY_S;
            case 203: return (int)CKKEY_A;
            case 205: return (int)CKKEY_D;
            default: return 0;
        }
    }

    static bool IsKeyEvent(CKBehavior *behavior) {
        return behavior && behavior->GetName() &&
               std::strcmp(behavior->GetName(), "Key Event") == 0;
    }

    bool KeyInputAlreadyBound(CKParameterIn *input) const {
        for (auto &key : m_Keys) {
            if (key.input == input) return true;
        }
        return false;
    }

    bool CutKeyWire(CKBehavior *keyBehavior, int code, int wasd) {
        CKParameterIn *input = keyBehavior->GetInputParameter(0);
        if (!input) return false;
        if (KeyInputAlreadyBound(input)) return false;

        CKParameter *direct = input->GetDirectSource();
        if (!direct) {
            GetLogger()->Info("[FFPC] key event has no direct source, skipped");
            return false;
        }

        char paramName[32];
        std::snprintf(paramName, sizeof(paramName), "FFPC_Key_%d", (int)m_Keys.size());

        CKParameterLocal *own =
                ScriptHelper::CreateLocalParameter(BallNavigation(), paramName, CKPGUID_INT);
        if (!own) {
            GetLogger()->Error("[FFPC] failed to create key parameter");
            return false;
        }

        const int value = m_WasdMode ? wasd : code;
        own->SetValue(&value, sizeof(value));
        input->SetDirectSource(own);

        KeyLink link;
        link.input = input;
        link.original = direct;
        link.own = own;
        link.originalCode = code;
        link.wasdCode = wasd;
        link.boundCode = value;
        link.name = direct->GetName() ? direct->GetName() : "";
        m_Keys.push_back(link);

        GetLogger()->Info("[FFPC] key '%s' (%d) detached -> %s (%d)", link.name.c_str(), code,
                          m_WasdMode ? "WASD" : "original", value);
        return true;
    }

    // The level script writes the bindings a few frames after StartLevel, so the
    // lookup is repeated until all four movement keys show up.
    void SetupKeyBindings() {
        if (m_KeysReady) return;
        CKBehavior *ballNav = BallNavigation();
        if (!ballNav) return;

        for (int i = 0; i < ballNav->GetSubBehaviorCount(); ++i) {
            CKBehavior *behavior = ballNav->GetSubBehavior(i);
            if (!IsKeyEvent(behavior)) continue;

            CKParameterIn *input = behavior->GetInputParameter(0);
            if (!input || KeyInputAlreadyBound(input)) continue;

            CKParameter *source = input->GetRealSource();
            if (!source) continue;

            int code = 0;
            if (source->GetValue(&code) != CK_OK) continue;

            const int wasd = WasdForKeyCode(code);
            if (wasd == 0) continue;

            CutKeyWire(behavior, code, wasd);
        }

        if (m_Keys.size() >= 4) {
            m_KeysReady = true;
            GetLogger()->Info("[FFPC] key bindings ready: %d inputs detached", (int)m_Keys.size());
        }
    }

    void UpdateKeyValues() {
        for (auto &key : m_Keys) {
            if (!key.own) continue;
            const int value = m_WasdMode ? key.wasdCode : key.originalCode;
            key.own->SetValue(&value, sizeof(value));
            key.boundCode = value;
        }
    }

    void RestoreHooks() {
        CKContext *ctx = Ctx();

        for (auto &key : m_Keys) {
            if (key.input && key.original) {
                key.input->SetDirectSource(key.original);
            }
        }
        if (ctx) {
            for (auto &key : m_Keys) {
                if (key.own) ctx->DestroyObject(key.own);
            }
        }
        m_Keys.clear();
        m_KeysReady = false;

        RestoreForceReference();
    }

    void EnsureHooks() {
        if (!m_KeysReady) {
            SetupKeyBindings();
        }
        VerifyKeyWires();
        if (!m_RefReady) {
            SetupForceReference();
        } else {
            ReassertForceReference();
        }
        TrackExtraKeys();
    }

    // The keys behind the remaining forces are never remapped, so a press of one
    // is logged here and can be tied to whatever the ball does right after.
    void TrackExtraKeys() {
        InputHook *input = BML()->GetInputManager();
        if (!input || m_ExtraKeys.empty()) return;
        if (m_ExtraKeyDown.size() != m_ExtraKeys.size()) {
            m_ExtraKeyDown.assign(m_ExtraKeys.size(), 0);
        }
        for (size_t i = 0; i < m_ExtraKeys.size(); ++i) {
            const int code = m_ExtraKeys[i];
            const bool down = code != 0 && input->IsKeyDown((CKDWORD)code) != FALSE;
            const bool was = m_ExtraKeyDown[i] != 0;
            if (down == was) continue;
            m_ExtraKeyDown[i] = down ? 1 : 0;
            GetLogger()->Info("[FFPC] untouched key %d %s", code,
                              down ? "pressed" : "released");
        }
    }

    // ------------------------------------------------------ level camera hook
    // The level turns its own camera through "Cam Navigation" -> a rotate
    // behavior -> "Bezier Progression" -> the camera rig, and the movement force
    // reads that rig.  Feeding the rotate behavior is therefore the way to make
    // the force follow the view.
    void FindGameCameraBBs() {
        m_RotateBB = nullptr;
        m_InterpBB = nullptr;
        m_CameraHookReady = false;
        m_HookCalls = 0;

        CKBehavior *nav = BML()->GetScriptByName("Gameplay_Ingame");
        if (nav) nav = ScriptHelper::FindFirstBB(nav, "Cam Navigation");
        if (!nav) {
            GetLogger()->Info("[FFPC] camera hook: Cam Navigation not found");
            return;
        }

        m_InterpBB = ScriptHelper::FindFirstBB(nav, "Bezier Progression");
        if (!m_InterpBB) {
            GetLogger()->Info("[FFPC] camera hook: Bezier Progression not found");
            return;
        }

        m_RotateBB = ScriptHelper::FindPreviousBB(nav, m_InterpBB->GetInput(0));
        if (!m_RotateBB) {
            GetLogger()->Info("[FFPC] camera hook: rotate behavior not found");
            return;
        }

        // The game's own camera rotation writes this same parameter.  Remember
        // its neutral value and put it back when first person is released;
        // otherwise the stale mouse rotation can eat the game's Shift+Left.
        if (CKParameterIn *rotation = m_RotateBB->GetInputParameter(0)) {
            if (CKParameter *source = rotation->GetDirectSource()) {
                if (source->GetValue(m_DefaultCameraRotation, sizeof(m_DefaultCameraRotation)) ==
                    CK_OK) {
                    m_HasDefaultCameraRotation = true;
                }
            }
        }

        // Remember the level's own interpolation duration, but leave it alone
        // here: it belongs to the game's third-person camera rotation too.
        // It is forced to zero only while first person is actually active
        // (see ApplyGameCameraDurationZero) and restored in Deactivate.
        if (CKParameterIn *duration = m_InterpBB->GetInputParameter(0)) {
            if (CKParameter *source = duration->GetDirectSource()) {
                if (source->GetValue(&m_DefaultDuration, sizeof(float)) == CK_OK) {
                    m_HasDefaultDuration = true;
                    m_DurationOverridden = false;
                }
            }
        }

        m_CameraHookReady = true;
        GetLogger()->Info("[FFPC] camera hook ready: rotate='%s' interp='%s' duration=%.3f (kept)",
                          m_RotateBB->GetName() ? m_RotateBB->GetName() : "?",
                          m_InterpBB->GetName() ? m_InterpBB->GetName() : "?",
                          m_HasDefaultDuration ? m_DefaultDuration : 0.0f);
    }

    // The level camera is turned by the same amount the view turns.  It is no
    // longer what the movement force reads - our own referential is - but the
    // level re-evaluates its movement forces when its camera rotates, so this is
    // what keeps the force in step with the view frame by frame.
    //
    // The rotation input is in radians (FreeViewRotation writes mouse pixels
    // times sensitivity divided by the width, which is a radian value); component
    // 0 is the pitch, which shows up as a shorter angle in the opposite
    // direction, component 1 is the yaw.
    //
    // In non-360 mode the force is horizontal, so the level camera only needs to
    // yaw: feeding pitch would make it drift past the view's pitch limit for no
    // benefit.  In 360 mode pitch is required because the force follows the full
    // view direction.
    void DriveGameCamera(const VxVector &delta) {
        if (!m_CameraHookReady || !m_RotateBB) return;
        if (delta.x == 0.0f && delta.y == 0.0f) return;
        if (!m_360Mode && delta.x == 0.0f) return;

        CKParameterIn *input = m_RotateBB->GetInputParameter(0);
        if (!input) return;
        CKParameter *source = input->GetDirectSource();
        if (!source) return;

        const float radiansPerPixel = m_Sensitivity * 0.0012f;
        const float yaw = delta.x * radiansPerPixel * (m_InvertX ? -1.0f : 1.0f);
        const float pitch = m_360Mode
                ? delta.y * radiansPerPixel * (m_InvertY ? -1.0f : 1.0f)
                : 0.0f;

        float rotation[3] = {pitch, yaw, 0.0f};
        source->SetValue(rotation, 3 * sizeof(float));
        m_RotateBB->ActivateInput(0);
        m_RotateBB->Activate();

        ++m_HookCalls;
        m_LastHookYaw = yaw;
        m_LastHookPitch = pitch;
    }

    // Yaw, pitch and roll of the level camera relative to the view, for the
    // diagnostics only: the movement force follows the view referential.
    bool ReadGameCameraErrors(float &yawError, float &pitchError, float &rollError) const {
        CK3dEntity *ref = GameRef();
        if (!ref) return false;

        const VxMatrix &wm = ref->GetWorldMatrix();
        const VxVector refForward(wm[2].x, wm[2].y, wm[2].z);
        const VxVector refRight(wm[0].x, wm[0].y, wm[0].z);

        yawError = WrapRadians(ViewYaw() - std::atan2f(refForward.x, refForward.z));
        pitchError = ViewPitch() - std::asinf(std::min(1.0f, std::max(-1.0f, refForward.y)));

        const VxVector forward = Forward();
        const VxVector ourRight = Right();
        const VxVector turn = Cross(refRight, ourRight);
        const float sine = turn.x * forward.x + turn.y * forward.y + turn.z * forward.z;
        const float cosine =
                refRight.x * ourRight.x + refRight.y * ourRight.y + refRight.z * ourRight.z;
        rollError = std::atan2f(sine, cosine);
        return true;
    }

    // The rotation only completes after the key animation with a non-zero
    // duration; first person needs it instant so DriveGameCamera takes effect
    // immediately.  This is only applied while first person is active.
    void ApplyGameCameraDurationZero() {
        if (!m_HasDefaultDuration || !m_InterpBB) return;
        if (CKParameterIn *duration = m_InterpBB->GetInputParameter(0)) {
            if (CKParameter *source = duration->GetDirectSource()) {
                float zero = 0.0f;
                source->SetValue(&zero, sizeof(zero));
                m_DurationOverridden = true;
            }
        }
    }

    void RestoreGameCameraDuration() {
        if (!m_HasDefaultDuration || !m_InterpBB) return;
        if (CKParameterIn *duration = m_InterpBB->GetInputParameter(0)) {
            if (CKParameter *source = duration->GetDirectSource()) {
                source->SetValue(&m_DefaultDuration, sizeof(float));
            }
        }
        m_DurationOverridden = false;
    }

    // Ball velocity from the position difference between two frames, for the
    // "does the movement follow the view" diagnostics.
    void UpdateBallMotion() {
        CK3dEntity *ball = CurrentBall();
        if (!ball) {
            m_HasBallPos = false;
            return;
        }
        VxVector pos(0.0f, 0.0f, 0.0f);
        ball->GetPosition(&pos);
        if (m_HasBallPos) {
            m_BallVelocity = pos - m_LastBallPos;
        } else {
            m_BallVelocity = VxVector(0.0f, 0.0f, 0.0f);
            m_HasBallPos = true;
        }
        m_LastBallPos = pos;
    }

    // If the level script re-plugs a Key Event input at any point, put our
    // parameter back.  Four pointer comparisons per frame, no writes unless the
    // game actually reconnected something.
    void VerifyKeyWires() {
        for (auto &key : m_Keys) {
            if (!key.input || !key.own) continue;
            if (key.input->GetDirectSource() == key.own) continue;
            key.input->SetDirectSource(key.own);
            ++m_KeyFixes;
            if (m_KeyFixes <= 8) {
                GetLogger()->Info("[FFPC] key '%s' re-detached (#%u)", key.name.c_str(), m_KeyFixes);
            }
        }
    }

    // ------------------------------------------------------------- lifecycle
    bool Activate() {
        if (m_Active) return true;
        if (!EnsureRig()) return false;

        CKRenderContext *render = Render();
        CKTargetCamera *cam = Camera();
        CKTargetCamera *gameCam = BML()->GetTargetCameraByName("InGameCam");
        if (!render || !cam || !gameCam) return false;

        EnsureHooks();
        ApplyGameCameraDurationZero();

        CKCamera *current = render->GetAttachedCamera();
        if (!current) {
            CK3dEntity *viewpoint = render->GetViewpoint();
            if (viewpoint && CKIsChildClassOf(viewpoint, CKCID_CAMERA)) {
                current = static_cast<CKCamera *>(viewpoint);
            }
        }
        if (current && current->GetID() != m_CameraId) {
            m_ViewCameraId = current->GetID();
            m_HasViewCamera = true;
        } else {
            m_HasViewCamera = false;
        }

        if (current && current->GetID() != m_CameraId) {
            m_SavedCameraId = current->GetID();
            m_HasSavedCamera = true;
        } else {
            m_HasSavedCamera = false;
        }

        UpdateCamera();
        render->AttachViewpointToCamera(cam);
        m_Active = true;

        if (m_ShowCursor) {
            if (InputHook *input = BML()->GetInputManager()) {
                m_CursorWasVisible = input->GetCursorVisibility() != FALSE;
                input->ShowCursor(TRUE);
            }
        }

        UpdateCursorLock();

        GetLogger()->Info("[FFPC] first-person active (saved camera id=%u)",
                          (unsigned)m_SavedCameraId);
        return true;
    }

    void Deactivate(const char *reason) {
        if (!m_Active) return;
        m_Active = false;

        CKRenderContext *render = Render();
        if (render) {
            CKCamera *restore = nullptr;
            if (m_HasSavedCamera) {
                CKObject *o = Ctx() ? Ctx()->GetObject(m_SavedCameraId) : nullptr;
                if (o && CKIsChildClassOf(o, CKCID_CAMERA)) {
                    restore = static_cast<CKCamera *>(o);
                }
            }
            if (!restore) restore = BML()->GetTargetCameraByName("InGameCam");
            if (restore) {
                render->AttachViewpointToCamera(restore);
            } else {
                render->DetachViewpointFromCamera();
            }
        }
        m_HasSavedCamera = false;

        if (m_ShowCursor) {
            if (InputHook *input = BML()->GetInputManager()) {
                if (!m_CursorWasVisible) input->ShowCursor(FALSE);
            }
        }

        RestoreGameCameraDuration();
        // The mouse look drives the level camera through input 0 of the camera
        // selector.  Restore the neutral rotation value and clear the latched
        // input, otherwise the game's own Shift+Left (same path) stays dead.
        if (m_RotateBB) {
            if (m_HasDefaultCameraRotation) {
                if (CKParameterIn *rotation = m_RotateBB->GetInputParameter(0)) {
                    if (CKParameter *source = rotation->GetDirectSource()) {
                        source->SetValue(m_DefaultCameraRotation,
                                         sizeof(m_DefaultCameraRotation));
                    }
                }
            }
            m_RotateBB->ActivateInput(0, FALSE);
        }
        RestoreHooks();
        UpdateCursorLock();
        GetLogger()->Info("[FFPC] first-person inactive (%s)", reason ? reason : "");
    }

    void KeepAttached() {
        CKRenderContext *render = Render();
        CKTargetCamera *cam = Camera();
        if (render && cam && render->GetAttachedCamera() != cam) {
            render->AttachViewpointToCamera(cam);
        }
    }

    // ------------------------------------------------------------ cursor lock
    // ShowCursor off means "no cursor in the way": hide it and pin the OS
    // pointer to the centre of the client area, so mouse look can never walk the
    // pointer out of the window and click something else.  Pausing (ESC),
    // losing focus and leaving the level release everything again.
    HWND GameWindow() const {
        CKContext *ctx = Ctx();
        return ctx ? static_cast<HWND>(ctx->GetMainWindow()) : nullptr;
    }

    bool IsGameForeground() const {
        HWND window = GameWindow();
        if (!window) return false;
        HWND foreground = GetForegroundWindow();
        if (!foreground) return false;
        return foreground == window ||
               GetAncestor(foreground, GA_ROOT) == GetAncestor(window, GA_ROOT);
    }

    void PinCursorToCentre() {
        HWND window = GameWindow();
        if (!window) return;

        RECT client = {0, 0, 0, 0};
        if (!GetClientRect(window, &client)) return;
        if (client.right <= client.left || client.bottom <= client.top) return;

        POINT centre = {(client.left + client.right) / 2, (client.top + client.bottom) / 2};
        if (!ClientToScreen(window, &centre)) return;
        SetCursorPos(centre.x, centre.y);
    }

    bool WantsCursorLock() const {
        return m_Enabled && m_Active && !m_ShowCursor && !m_Paused && BML()->IsPlaying() &&
               !BML()->IsPaused() && IsGameForeground();
    }

    void ReleaseCursorLock(bool showForMenu) {
        if (m_OsCursorHidden) {
            SetCursor(m_SavedOsCursor ? m_SavedOsCursor : LoadCursor(nullptr, IDC_ARROW));
            m_OsCursorHidden = false;
        }
        if (m_GameCursorHidden) {
            if (InputHook *input = BML()->GetInputManager()) {
                input->ShowCursor(TRUE);
            }
            m_GameCursorHidden = false;
        }
        if (m_CursorLocked) {
            m_CursorLocked = false;
            if (m_Debug) {
                GetLogger()->Info("[FFPC] cursor released (%s)",
                                  showForMenu ? "menu" : "leaving play");
            }
        }
    }

    void UpdateCursorLock() {
        if (!WantsCursorLock()) {
            const bool menu = m_Paused || BML()->IsPaused();
            ReleaseCursorLock(menu);
            return;
        }

        if (!m_CursorLocked) {
            m_CursorLocked = true;
            if (InputHook *input = BML()->GetInputManager()) {
                input->ShowCursor(FALSE);
                m_GameCursorHidden = true;
            }
            m_SavedOsCursor = GetCursor();
            m_OsCursorHidden = true;
            if (m_Debug) {
                GetLogger()->Info("[FFPC] cursor hidden and pinned to the centre");
            }
        }

        // The game may show its own cursor again; keep it hidden while playing.
        if (m_GameCursorHidden) {
            if (InputHook *input = BML()->GetInputManager()) {
                if (input->GetCursorVisibility()) input->ShowCursor(FALSE);
            }
        }

        PinCursorToCentre();

        // SetCursor(NULL) hides the OS pointer without touching the ShowCursor
        // counter, so it cannot drift against the game or an ImGui overlay.
        SetCursor(nullptr);
    }

    // ---------------------------------------------------------------- camera
    // The view is stored as an orthonormal basis instead of yaw/pitch angles so
    // that 360 mode can rotate around the view's own up axis.  Yaw/pitch are
    // only derived for logging.
    VxVector Forward() const { return m_Forward; }
    VxVector Up() const { return m_Up; }

    VxVector Right() const {
        VxVector right = Cross(m_Up, m_Forward);
        right.Normalize();
        return right;
    }

    float ViewYaw() const { return std::atan2f(m_Forward.x, m_Forward.z); }
    float ViewPitch() const {
        float y = m_Forward.y;
        if (y > 1.0f) y = 1.0f;
        if (y < -1.0f) y = -1.0f;
        return std::asinf(y);
    }

    static VxVector RotateAxis(const VxVector &v, const VxVector &axis, float angle) {
        const float c = std::cosf(angle);
        const float s = std::sinf(angle);
        const float d = v.x * axis.x + v.y * axis.y + v.z * axis.z;
        const VxVector k(v.y * axis.z - v.z * axis.y, v.z * axis.x - v.x * axis.z,
                         v.x * axis.y - v.y * axis.x);
        return VxVector(v.x * c + k.x * s + axis.x * d * (1.0f - c),
                        v.y * c + k.y * s + axis.y * d * (1.0f - c),
                        v.z * c + k.z * s + axis.z * d * (1.0f - c));
    }

    void NormalizeBasis() {
        m_Forward.Normalize();
        const float d = m_Up.x * m_Forward.x + m_Up.y * m_Forward.y + m_Up.z * m_Forward.z;
        m_Up.Set(m_Up.x - m_Forward.x * d, m_Up.y - m_Forward.y * d, m_Up.z - m_Forward.z * d);
        m_Up.Normalize();
    }

    void SetViewFromYaw(float yaw) {
        m_Forward.Set(std::sinf(yaw), 0.0f, std::cosf(yaw));
        m_Up.Set(0.0f, 1.0f, 0.0f);
        NormalizeBasis();
    }

    void ApplyPitch(float delta) {
        if (delta == 0.0f) return;
        const VxVector right = Right();
        m_Forward = RotateAxis(m_Forward, right, delta);
        m_Up = RotateAxis(m_Up, right, delta);
    }

    void ApplyYaw(float delta) {
        if (delta == 0.0f) return;
        const VxVector axis = m_360Mode ? m_Up : VxVector(0.0f, 1.0f, 0.0f);
        m_Forward = RotateAxis(m_Forward, axis, delta);
        m_Up = RotateAxis(m_Up, axis, delta);
    }

    // In limited mode the pitch never goes past +/-kPitchLimit.
    float ClampPitchDelta(float delta) const {
        if (m_360Mode || delta == 0.0f) return delta;
        const float pitch = ViewPitch();
        if (delta > 0.0f) {
            const float room = kPitchLimit - pitch;
            return std::min(delta, std::max(0.0f, room));
        }
        const float room = -kPitchLimit - pitch;
        return std::max(delta, std::min(0.0f, room));
    }

    // Normalises a view that may come from 360 mode (pitch past +/-90, roll)
    // into the non-360 model: yaw is preserved, pitch is clamped to +/-85 and
    // roll is removed by rebuilding the basis from world up.
    void ClampViewToPitchLimit() {
        if (m_360Mode) return;

        const float yaw = ViewYaw();
        const float y = std::min(1.0f, std::max(-1.0f, m_Forward.y));
        float pitch = std::asinf(y);
        // Past vertical (and not merely a rolled level view): asin returns the
        // supplement of the real angle.  The horizontal guard keeps a 180
        // degree roll at level pitch from being mistaken for an upside-down
        // view.
        if (m_Up.y < 0.0f && std::fabs(y) > 0.1f) {
            pitch = (pitch >= 0.0f) ? (kPi - pitch) : (-kPi - pitch);
        }
        if (pitch > kPitchLimit) pitch = kPitchLimit;
        if (pitch < -kPitchLimit) pitch = -kPitchLimit;

        SetViewFromYaw(yaw);
        ApplyPitch(pitch);
        NormalizeBasis();
    }

    // Horizontal direction from InGameCam towards the ball, i.e. the direction
    // the game's own camera looks in.
    bool ComputeGameView(VxVector &out) const {
        CK3dEntity *ball = CurrentBall();
        CKTargetCamera *gameCam = BML()->GetTargetCameraByName("InGameCam");
        if (!ball || !gameCam) return false;

        VxVector ballPos(0.0f, 0.0f, 0.0f);
        VxVector camPos(0.0f, 0.0f, 0.0f);
        ball->GetPosition(&ballPos);
        gameCam->GetPosition(&camPos);

        const float dx = ballPos.x - camPos.x;
        const float dz = ballPos.z - camPos.z;
        if (dx * dx + dz * dz < 4.0f) return false;

        out.Set(dx, 0.0f, dz);
        out.Normalize();
        return true;
    }

    bool LockViewFromCamera(const char *reason) {
        VxVector dir;
        if (!ComputeGameView(dir)) return false;

        SetViewFromYaw(std::atan2f(dir.x, dir.z));
        m_ViewLocked = true;
        GetLogger()->Info("[FFPC] view locked: yaw=%.3f dir=(%.2f,%.2f,%.2f) reason=%s", ViewYaw(),
                          dir.x, dir.y, dir.z, reason);
        LogPose("locked");
        return true;
    }

    void FollowGameCamera() {
        if (m_ViewLocked || !m_ViewFollowing) return;
        VxVector dir;
        if (!ComputeGameView(dir)) return;
        SetViewFromYaw(std::atan2f(dir.x, dir.z));
    }

    void ReadMouseLook(const VxVector &delta) {
        if (delta.x == 0.0f && delta.y == 0.0f) return;

        if (!m_ViewLocked) {
            m_ViewLocked = true;
            GetLogger()->Info("[FFPC] view locked: player mouse input");
        }

        const float scale = m_Sensitivity * 0.0012f;
        // RotateAxis turns the opposite way around the given axis, so the yaw
        // delta is negated to keep "mouse right = turn right".
        const float yawDelta = -delta.x * scale * (m_InvertX ? -1.0f : 1.0f);
        const float pitchDelta = -delta.y * scale * (m_InvertY ? -1.0f : 1.0f);

        ApplyYaw(yawDelta);
        ApplyPitch(ClampPitchDelta(pitchDelta));
        NormalizeBasis();
    }

    void UpdateCamera() {
        CKTargetCamera *cam = Camera();
        CK3dEntity *ball = CurrentBall();
        if (!cam || !ball) return;

        VxVector ballPos(0.0f, 0.0f, 0.0f);
        ball->GetPosition(&ballPos);
        VxVector pos(ballPos.x, ballPos.y, ballPos.z);

        const VxVector forward = Forward();
        const VxVector up = Up();
        cam->SetPosition(pos);
        cam->SetOrientation(forward, up);

        if (CKTargetCamera *gameCam = BML()->GetTargetCameraByName("InGameCam")) {
            int width = 0;
            int height = 0;
            gameCam->GetAspectRatio(width, height);
            if (width > 0 && height > 0) cam->SetAspectRatio(width, height);
            cam->SetFov(gameCam->GetFov());

            // Follow the camera that actually renders the game view.  On old
            // BML, the object returned by GetTargetCameraByName("InGameCam") is
            // not always the attached camera, so ViewCamera() prefers the
            // active camera captured in Activate().
            CKCamera *viewCam = ViewCamera();
            const float back = viewCam ? viewCam->GetBackPlane() : 0.0f;
            const float effectiveBack = std::max(back, m_ViewDistance);
            if (effectiveBack > 0.0f) cam->SetBackPlane(effectiveBack);
            cam->SetFrontPlane(kNearPlane);
        }
    }

    // ------------------------------------------------------------------- log
    void LogPose(const char *tag) {
        CK3dEntity *ball = CurrentBall();
        CKTargetCamera *gameCam = BML()->GetTargetCameraByName("InGameCam");
        if (!ball || !gameCam) {
            GetLogger()->Info("[FFPC] %s ball=? cam=?", tag);
            return;
        }

        VxVector ballPos(0.0f, 0.0f, 0.0f);
        VxVector camPos(0.0f, 0.0f, 0.0f);
        ball->GetPosition(&ballPos);
        gameCam->GetPosition(&camPos);

        VxVector dir(0.0f, 0.0f, 0.0f);
        const bool valid = ComputeGameView(dir);
        GetLogger()->Info("[FFPC] %s ball=(%.2f,%.2f,%.2f) game=(%.2f,%.2f,%.2f) yaw=%.3f valid=%d",
                          tag, ballPos.x, ballPos.y, ballPos.z, camPos.x, camPos.y, camPos.z,
                          valid ? std::atan2f(dir.x, dir.z) : 0.0f, valid ? 1 : 0);
    }

    void LogState() {
        CK3dEntity *ball = CurrentBall();
        CKTargetCamera *cam = Camera();
        CKTargetCamera *gameCam = BML()->GetTargetCameraByName("InGameCam");
        if (!ball || !cam || !gameCam) return;

        VxVector ballPos(0.0f, 0.0f, 0.0f);
        VxVector camPos(0.0f, 0.0f, 0.0f);
        VxVector gamePos(0.0f, 0.0f, 0.0f);
        ball->GetPosition(&ballPos);
        cam->GetPosition(&camPos);
        gameCam->GetPosition(&gamePos);
        const VxVector forward = Forward();

        int keys = 0;
        if (InputHook *input = BML()->GetInputManager()) {
            if (input->IsKeyDown(CKKEY_UP)) keys |= 1;
            if (input->IsKeyDown(CKKEY_DOWN)) keys |= 2;
            if (input->IsKeyDown(CKKEY_LEFT)) keys |= 4;
            if (input->IsKeyDown(CKKEY_RIGHT)) keys |= 8;
            if (input->IsKeyDown(CKKEY_W)) keys |= 16;
            if (input->IsKeyDown(CKKEY_A)) keys |= 32;
            if (input->IsKeyDown(CKKEY_S)) keys |= 64;
            if (input->IsKeyDown(CKKEY_D)) keys |= 128;
        }

        GetLogger()->Info(
                "[FFPC] ball=(%.2f,%.2f,%.2f) cam=(%.2f,%.2f,%.2f) game=(%.2f,%.2f,%.2f) "
                "view=(%.2f,%.2f,%.2f) yaw=%.2f pitch=%.2f keys=%d active=%d locked=%d "
                "follow=%d fix=%u mode=%d cursor=%d temp=%d viewdist=%.0f near=%.3f gamenear=%.3f",
                ballPos.x, ballPos.y, ballPos.z, camPos.x, camPos.y, camPos.z, gamePos.x,
                gamePos.y, gamePos.z, forward.x, forward.y, forward.z, ViewYaw(), ViewPitch(), keys,
                m_Active ? 1 : 0, m_ViewLocked ? 1 : 0, m_ViewFollowing ? 1 : 0, m_KeyFixes,
                m_360Mode ? 1 : 0, m_CursorLocked ? 1 : 0, m_TempOn ? 1 : 0, cam->GetBackPlane(),
                cam->GetFrontPlane(), gameCam->GetFrontPlane());

        // How well the ball is moving where the force points.  In non-360 mode
        // the force is horizontal, so compare against the flattened view instead
        // of the (possibly pitched) view direction.
        const float toDeg = 180.0f / kPi;
        const float speed = m_BallVelocity.Magnitude();
        VxVector vel(0.0f, 0.0f, 0.0f);
        if (speed > 1e-7f) {
            vel = VxVector(m_BallVelocity.x / speed, m_BallVelocity.y / speed,
                           m_BallVelocity.z / speed);
        }
        VxVector forceForward, forceUp;
        ForceBasis(forceForward, forceUp);
        (void)forceUp;
        GetLogger()->Info("[FFPC] follow dot=%.3f view=(%.1f,%.1f) spd=%.3f ref=%d",
                          vel.x * forceForward.x + vel.y * forceForward.y +
                                  vel.z * forceForward.z,
                          ViewYaw() * toDeg, ViewPitch() * toDeg, speed, m_RefReady ? 1 : 0);

        // Difference between the referential the force uses now (ours) and the
        // one the level would use on its own.  In non-360 mode ours is flattened
        // to yaw, so a non-zero dForward against a pitched level reference is
        // expected; dRoll should stay at zero.
        LogReferenceComparison();
    }

    void LogReferenceComparison() {
        const float toDeg = 180.0f / kPi;
        VxVector ourForward, ourUp;
        ForceBasis(ourForward, ourUp);
        VxVector ourRight = Cross(ourUp, ourForward);
        ourRight.Normalize();

        VxVector theirForward(0.0f, 0.0f, 1.0f);
        VxVector theirRight(1.0f, 0.0f, 0.0f);
        bool found = false;
        if (CK3dEntity *ref = GameRef()) {
            const VxMatrix &wm = ref->GetWorldMatrix();
            theirForward = VxVector(wm[2].x, wm[2].y, wm[2].z);
            theirRight = VxVector(wm[0].x, wm[0].y, wm[0].z);
            found = true;
        }

        const float dotForward = std::min(1.0f, std::max(-1.0f,
                ourForward.x * theirForward.x + ourForward.y * theirForward.y +
                ourForward.z * theirForward.z));
        const VxVector turn = Cross(theirRight, ourRight);
        const float sine = turn.x * ourForward.x + turn.y * ourForward.y + turn.z * ourForward.z;
        const float cosine = theirRight.x * ourRight.x + theirRight.y * ourRight.y +
                             theirRight.z * ourRight.z;

        GetLogger()->Info(
                "[FFPC] refcheck found=%d dForward=%.2f dRoll=%.2f theirPitch=%.2f "
                "ours=(%.3f,%.3f,%.3f) theirs=(%.3f,%.3f,%.3f)",
                found ? 1 : 0, std::acosf(dotForward) * toDeg, std::atan2f(sine, cosine) * toDeg,
                std::asinf(std::min(1.0f, std::max(-1.0f, theirForward.y))) * toDeg,
                ourForward.x, ourForward.y, ourForward.z, theirForward.x, theirForward.y,
                theirForward.z);
    }

    // ---------------------------------------------------------------- state
    IProperty *m_EnabledProp = nullptr;
    IProperty *m_ToggleKeyProp = nullptr;
    IProperty *m_SensitivityProp = nullptr;
    IProperty *m_InvertXProp = nullptr;
    IProperty *m_InvertYProp = nullptr;
    IProperty *m_360ModeProp = nullptr;
    IProperty *m_WasdModeProp = nullptr;
    IProperty *m_ShowCursorProp = nullptr;
    IProperty *m_DebugProp = nullptr;

    bool m_Enabled = true;
    bool m_TempOn = true;
    CKKEYBOARD m_ToggleKey = CKKEY_F;
    float m_Sensitivity = 4.0f;
    bool m_InvertX = false;
    bool m_InvertY = false;
    bool m_360Mode = false;
    bool m_WasdMode = true;
    bool m_ShowCursor = true;
    bool m_Debug = true;
    float m_ViewDistance = 0.0f;

    CK_ID m_CameraId = 0;
    CK_ID m_GameRefId = 0;
    CK_ID m_ForceRefId = 0;
    CK_ID m_SavedCameraId = 0;
    CK_ID m_ViewCameraId = 0;
    bool m_HasSavedCamera = false;
    bool m_HasViewCamera = false;
    bool m_Active = false;
    bool m_ViewLocked = false;
    bool m_ViewFollowing = false;
    bool m_RespawnPending = false;
    bool m_BallActive = false;
    bool m_CursorWasVisible = true;
    bool m_Paused = false;
    bool m_CursorLocked = false;
    bool m_GameCursorHidden = false;
    bool m_OsCursorHidden = false;
    HCURSOR m_SavedOsCursor = nullptr;
    bool m_LoggedKeys = false;
    bool m_LoggedLayout = false;
    bool m_KeysReady = false;
    bool m_RefReady = false;
    VxVector m_Forward{0.0f, 0.0f, 1.0f};
    VxVector m_Up{0.0f, 1.0f, 0.0f};
    CKBehavior *m_RotateBB = nullptr;
    CKBehavior *m_InterpBB = nullptr;
    bool m_CameraHookReady = false;
    bool m_HasDefaultDuration = false;
    bool m_DurationOverridden = false;
    float m_DefaultDuration = 0.0f;
    bool m_HasDefaultCameraRotation = false;
    float m_DefaultCameraRotation[3] = {0.0f, 0.0f, 0.0f};
    unsigned m_HookCalls = 0;
    float m_LastHookYaw = 0.0f;
    float m_LastHookPitch = 0.0f;
    CKParameter *m_RefParam = nullptr;
    std::vector<RefLink> m_RefLinks;
    std::vector<int> m_ExtraKeys;
    std::vector<char> m_ExtraKeyDown;
    VxVector m_LastBallPos{0.0f, 0.0f, 0.0f};
    VxVector m_BallVelocity{0.0f, 0.0f, 0.0f};
    bool m_HasBallPos = false;
    std::vector<KeyLink> m_Keys;
    unsigned m_KeyFixes = 0;
    unsigned m_Frame = 0;
    IBML *m_BmlPtr = nullptr;
};

MOD_EXPORT IMod *BMLEntry(IBML *bml) {
    return new FreeFirstPersonCamera(bml);
}

MOD_EXPORT void BMLExit(IMod *mod) {
    delete mod;
}
