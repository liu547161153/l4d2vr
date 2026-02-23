// Mock OpenVR backend (no SteamVR/OpenVR API calls)
//
// Goal: allow running the mod's pose-driven logic and VR rendering paths on a non-VR machine.
// When UseMockVR=true, we generate synthetic poses (HMD + two controllers) and publish them
// as vr::TrackedDevicePose_t, so existing GetPoseData()/pose cache logic continues to work.

namespace
{
    inline bool MockKeyDown(int vk)
    {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    inline float MockWrapDeg(float a)
    {
        while (a > 180.0f) a -= 360.0f;
        while (a < -180.0f) a += 360.0f;
        return a;
    }

    inline float MockDeltaDeg(float cur, float prev)
    {
        return MockWrapDeg(cur - prev);
    }

    // Build OpenVR rotation matrix such that VR::GetPoseData() extracts the original QAngle.
    // Extraction in VR::GetPoseData():
    //   pitch = asin(m[1][2])
    //   yaw   = atan2(m[0][2], m[2][2])
    //   roll  = atan2(-m[1][0], m[1][1])
    // Matching construction (verified by brute force):
    //   R = Ry(yaw) * Rx(-pitch) * Rz(-roll)
    inline void MockBuildOpenVRRotation(const QAngle& angDeg, float outR[3][3])
    {
        const float deg2rad = 3.14159265358979323846f / 180.0f;
        const float pitch = -angDeg.x * deg2rad;
        const float yaw = angDeg.y * deg2rad;
        const float roll = -angDeg.z * deg2rad;

        const float cp = cosf(pitch), sp = sinf(pitch);
        const float cy = cosf(yaw), sy = sinf(yaw);
        const float cr = cosf(roll), sr = sinf(roll);

        // Ry
        const float Ry[3][3] = {
            { cy, 0.0f, sy },
            { 0.0f, 1.0f, 0.0f },
            { -sy, 0.0f, cy }
        };
        // Rx
        const float Rx[3][3] = {
            { 1.0f, 0.0f, 0.0f },
            { 0.0f, cp, -sp },
            { 0.0f, sp, cp }
        };
        // Rz
        const float Rz[3][3] = {
            { cr, -sr, 0.0f },
            { sr, cr, 0.0f },
            { 0.0f, 0.0f, 1.0f }
        };

        // outR = Ry * Rx * Rz
        float tmp[3][3]{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                tmp[r][c] = Ry[r][0] * Rx[0][c] + Ry[r][1] * Rx[1][c] + Ry[r][2] * Rx[2][c];

        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                outR[r][c] = tmp[r][0] * Rz[0][c] + tmp[r][1] * Rz[1][c] + tmp[r][2] * Rz[2][c];
    }

    // Source-local meters -> OpenVR tracking translation (meters)
    // VR::GetPoseData position mapping:
    //   Sx = -Oz, Sy = -Ox, Sz = Oy
    // => Ox = -Sy, Oy = Sz, Oz = -Sx
    inline void MockSourcePosToOpenVR(const Vector& posS, float& ox, float& oy, float& oz)
    {
        ox = -posS.y;
        oy = posS.z;
        oz = -posS.x;
    }

    inline void MockSourceVelToOpenVR(const Vector& velS, float& vx, float& vy, float& vz)
    {
        vx = -velS.y;
        vy = velS.z;
        vz = -velS.x;
    }

    // Source angular velocity (deg/s) -> OpenVR (rad/s)
    inline void MockSourceAngVelToOpenVR(const QAngle& angVelDegS, float& wx, float& wy, float& wz)
    {
        const float deg2rad = 3.14159265358979323846f / 180.0f;
        // VR::GetPoseData does:
        //   angvel.x = -wz * rad2deg
        //   angvel.y = -wx * rad2deg
        //   angvel.z = +wy * rad2deg
        // => wx = -angvel.y * deg2rad
        //    wy = +angvel.z * deg2rad
        //    wz = -angvel.x * deg2rad
        wx = -angVelDegS.y * deg2rad;
        wy = angVelDegS.z * deg2rad;
        wz = -angVelDegS.x * deg2rad;
    }

    inline vr::TrackedDevicePose_t MockMakePose(const Vector& posS, const QAngle& angS, const Vector& velS, const QAngle& angVelDegS)
    {
        vr::TrackedDevicePose_t p{};
        p.bDeviceIsConnected = true;
        p.bPoseIsValid = true;
        p.eTrackingResult = vr::TrackingResult_Running_OK;

        float R[3][3]{};
        MockBuildOpenVRRotation(angS, R);

        float ox, oy, oz;
        MockSourcePosToOpenVR(posS, ox, oy, oz);

        // Row-major 3x4
        p.mDeviceToAbsoluteTracking.m[0][0] = R[0][0];
        p.mDeviceToAbsoluteTracking.m[0][1] = R[0][1];
        p.mDeviceToAbsoluteTracking.m[0][2] = R[0][2];
        p.mDeviceToAbsoluteTracking.m[0][3] = ox;

        p.mDeviceToAbsoluteTracking.m[1][0] = R[1][0];
        p.mDeviceToAbsoluteTracking.m[1][1] = R[1][1];
        p.mDeviceToAbsoluteTracking.m[1][2] = R[1][2];
        p.mDeviceToAbsoluteTracking.m[1][3] = oy;

        p.mDeviceToAbsoluteTracking.m[2][0] = R[2][0];
        p.mDeviceToAbsoluteTracking.m[2][1] = R[2][1];
        p.mDeviceToAbsoluteTracking.m[2][2] = R[2][2];
        p.mDeviceToAbsoluteTracking.m[2][3] = oz;

        float vx, vy, vz;
        MockSourceVelToOpenVR(velS, vx, vy, vz);
        p.vVelocity.v[0] = vx;
        p.vVelocity.v[1] = vy;
        p.vVelocity.v[2] = vz;

        float wx, wy, wz;
        MockSourceAngVelToOpenVR(angVelDegS, wx, wy, wz);
        p.vAngularVelocity.v[0] = wx;
        p.vAngularVelocity.v[1] = wy;
        p.vAngularVelocity.v[2] = wz;

        return p;
    }

    inline void MockYawRotateOffset(const Vector& offsetLocal, float yawDeg, Vector& outWorld)
    {
        const float deg2rad = 3.14159265358979323846f / 180.0f;
        const float y = yawDeg * deg2rad;
        const float cy = cosf(y);
        const float sy = sinf(y);
        // Local offset uses Source convention: x=forward, y=left.
        outWorld.x = offsetLocal.x * cy - offsetLocal.y * sy;
        outWorld.y = offsetLocal.x * sy + offsetLocal.y * cy;
        outWorld.z = offsetLocal.z;
    }
}

void VR::InitMockVR()
{
    // Render target size defaults to current window size (fallback to 1920x1080).
    int w = 0, h = 0;
    if (m_Game && m_Game->m_MaterialSystem)
    {
        if (IMatRenderContext* ctx = m_Game->m_MaterialSystem->GetRenderContext())
        {
            ctx->GetWindowSize(w, h);
        }
    }

    if (w <= 0) w = 1920;
    if (h <= 0) h = 1080;

    m_RenderWidth = (uint32_t)w;
    m_RenderHeight = (uint32_t)h;
    m_AntiAliasing = 0;
    m_Aspect = (h > 0) ? ((float)w / (float)h) : 1.0f;
    m_Fov = 90.0f;

    // Full bounds for both eyes in mock mode.
    m_TextureBounds[0] = { 0.0f, 0.0f, 1.0f, 1.0f };
    m_TextureBounds[1] = { 0.0f, 0.0f, 1.0f, 1.0f };

    // Prime pose buffers.
    for (uint32_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i)
    {
        m_Poses[i] = {};
        m_MockPoseBuf[i] = {};
    }

    m_MockVRLastUpdate = std::chrono::steady_clock::time_point{};
    m_MockVRMouseInit = false;

    // Seed previous values for velocity estimation.
    m_MockVRPrevHmdPos = m_MockVRHmdPos;
    m_MockVRPrevHmdAng = m_MockVRHmdAng;

    // If hands follow the HMD, seed prev from computed position; otherwise from absolute.
    if (m_MockVRHandsFollowHmd)
    {
        Vector lOffW{}, rOffW{};
        MockYawRotateOffset(m_MockVRLeftHandOffset, m_MockVRHmdAng.y, lOffW);
        MockYawRotateOffset(m_MockVRRightHandOffset, m_MockVRHmdAng.y, rOffW);
        m_MockVRPrevLeftPos = m_MockVRHmdPos + lOffW;
        m_MockVRPrevRightPos = m_MockVRHmdPos + rOffW;
        m_MockVRPrevLeftAng = QAngle{ m_MockVRHmdAng.x + m_MockVRHandPitchOffset, m_MockVRHmdAng.y, 0.0f };
        m_MockVRPrevRightAng = QAngle{ m_MockVRHmdAng.x + m_MockVRHandPitchOffset, m_MockVRHmdAng.y, 0.0f };
    }
    else
    {
        m_MockVRPrevLeftPos = m_MockVRLeftHandPos;
        m_MockVRPrevRightPos = m_MockVRRightHandPos;
        m_MockVRPrevLeftAng = m_MockVRLeftHandAng;
        m_MockVRPrevRightAng = m_MockVRRightHandAng;
    }

    // Start config hot-reload thread (mock also benefits from live tuning).
    std::thread configParser(&VR::WaitForConfigUpdate, this);
    configParser.detach();

    // Wait for d3d9 wrapper, but do not touch SteamVR/OpenVR.
    while (!g_D3DVR9)
        Sleep(10);

    g_D3DVR9->GetBackBufferData(&m_VKBackBuffer);

    // Mark VR logic active.
    UpdateMockVRPoses();
    m_IsInitialized = true;
    m_IsVREnabled = true;

    Game::logMsg("[VR] Mock OpenVR backend active (UseMockVR=true). No SteamVR/OpenVR API calls will be made.");
}

vr::TrackedDeviceIndex_t VR::GetControllerIndexForRole(vr::ETrackedControllerRole role) const
{
    if (m_UseMockVR)
    {
        if (role == vr::TrackedControllerRole_LeftHand)
            return 1;
        if (role == vr::TrackedControllerRole_RightHand)
            return 2;
        return vr::k_unTrackedDeviceIndexInvalid;
    }

    if (!m_System)
        return vr::k_unTrackedDeviceIndexInvalid;

    return m_System->GetTrackedDeviceIndexForControllerRole(role);
}

void VR::UpdateMockVRPoses()
{
    if (!m_UseMockVR)
        return;

    const auto now = std::chrono::steady_clock::now();
    float dt = 1.0f / 90.0f;
    if (m_MockVRLastUpdate.time_since_epoch().count() != 0)
    {
        dt = std::chrono::duration<float>(now - m_MockVRLastUpdate).count();
        dt = std::clamp(dt, 0.000001f, 0.1f);
    }
    m_MockVRLastUpdate = now;

    // Reset mock poses to config-defined values.
    if (MockKeyDown(VK_HOME))
    {
        // VK_HOME might be held for multiple frames; only reset when it transitions is nicer,
        // but for simplicity we just do it continuously.
        m_MockVRMouseInit = false;
    }

    // Keyboard/mouse controls (optional):
    //  - Default: move HMD with WASD (ground plane) + PageUp/PageDown (vertical)
    //  - Mouse look (optional): updates HMD yaw/pitch
    //  - Hold Alt: move RIGHT hand instead of HMD
    //  - Hold Ctrl: move LEFT hand instead of HMD
    const bool useKBM = m_MockVRKeyboardMouse;

    Vector* moveTargetPos = &m_MockVRHmdPos;
    QAngle* moveTargetAng = &m_MockVRHmdAng;

    if (useKBM)
    {
        const bool alt = MockKeyDown(VK_MENU);
        const bool ctrl = MockKeyDown(VK_CONTROL);

        // Decide which pose we are editing.
        if (!m_MockVRHandsFollowHmd)
        {
            if (alt)
            {
                moveTargetPos = &m_MockVRRightHandPos;
                moveTargetAng = &m_MockVRRightHandAng;
            }
            else if (ctrl)
            {
                moveTargetPos = &m_MockVRLeftHandPos;
                moveTargetAng = &m_MockVRLeftHandAng;
            }
        }

        // Mouse look (HMD only)
        if (m_MockVRMouseLook)
        {
            const bool allowMouse = !m_MockVRMouseLookRequiresRMB || MockKeyDown(VK_RBUTTON);
            if (allowMouse)
            {
                POINT cur{};
                if (GetCursorPos(&cur))
                {
                    if (!m_MockVRMouseInit)
                    {
                        m_MockVRLastMouse = cur;
                        m_MockVRMouseInit = true;
                    }
                    else
                    {
                        const long dx = cur.x - m_MockVRLastMouse.x;
                        const long dy = cur.y - m_MockVRLastMouse.y;
                        m_MockVRLastMouse = cur;

                        // Only drive HMD angles with mouse (even if editing hands with modifier keys).
                        m_MockVRHmdAng.y = MockWrapDeg(m_MockVRHmdAng.y + (float)dx * m_MockVRMouseSensitivity);
                        m_MockVRHmdAng.x = std::clamp(m_MockVRHmdAng.x + (float)(-dy) * m_MockVRMouseSensitivity, -89.0f, 89.0f);
                    }
                }
            }
            else
            {
                m_MockVRMouseInit = false;
            }
        }

        // Movement
        const float baseSpeed = std::max(0.0f, m_MockVRMoveSpeed);
        const float speed = baseSpeed * (MockKeyDown(VK_SHIFT) ? std::max(1.0f, m_MockVRRunMultiplier) : 1.0f);

        Vector move{ 0.0f, 0.0f, 0.0f };

        const float yawRad = m_MockVRHmdAng.y * (3.14159265358979323846f / 180.0f);
        const float cy = cosf(yawRad);
        const float sy = sinf(yawRad);

        const Vector fwd{ cy, sy, 0.0f };          // +X forward, +Y left
        const Vector left{ -sy, cy, 0.0f };

        if (MockKeyDown('W')) move = move + fwd;
        if (MockKeyDown('S')) move = move - fwd;
        if (MockKeyDown('A')) move = move + left;
        if (MockKeyDown('D')) move = move - left;
        if (MockKeyDown(VK_PRIOR)) move.z += 1.0f; // PageUp
        if (MockKeyDown(VK_NEXT))  move.z -= 1.0f; // PageDown

        if (!move.IsZero())
        {
            // normalize
            const float len = sqrtf(move.x * move.x + move.y * move.y + move.z * move.z);
            if (len > 0.00001f)
            {
                move.x /= len; move.y /= len; move.z /= len;
                (*moveTargetPos).x += move.x * speed * dt;
                (*moveTargetPos).y += move.y * speed * dt;
                (*moveTargetPos).z += move.z * speed * dt;
                if ((*moveTargetPos).z < 0.0f) (*moveTargetPos).z = 0.0f;
            }
        }

        // Angle nudges for the currently selected target (arrow keys)
        const float keyYaw = 90.0f;   // deg/s
        const float keyPitch = 90.0f; // deg/s
        if (MockKeyDown(VK_LEFT))  (*moveTargetAng).y = MockWrapDeg((*moveTargetAng).y - keyYaw * dt);
        if (MockKeyDown(VK_RIGHT)) (*moveTargetAng).y = MockWrapDeg((*moveTargetAng).y + keyYaw * dt);
        if (MockKeyDown(VK_UP))    (*moveTargetAng).x = std::clamp((*moveTargetAng).x - keyPitch * dt, -89.0f, 89.0f);
        if (MockKeyDown(VK_DOWN))  (*moveTargetAng).x = std::clamp((*moveTargetAng).x + keyPitch * dt, -89.0f, 89.0f);
    }

    // Compute hand poses.
    Vector leftPos = m_MockVRLeftHandPos;
    Vector rightPos = m_MockVRRightHandPos;
    QAngle leftAng = m_MockVRLeftHandAng;
    QAngle rightAng = m_MockVRRightHandAng;

    if (m_MockVRHandsFollowHmd)
    {
        Vector lOffW{}, rOffW{};
        MockYawRotateOffset(m_MockVRLeftHandOffset, m_MockVRHmdAng.y, lOffW);
        MockYawRotateOffset(m_MockVRRightHandOffset, m_MockVRHmdAng.y, rOffW);
        leftPos = m_MockVRHmdPos + lOffW;
        rightPos = m_MockVRHmdPos + rOffW;

        leftAng = QAngle{ m_MockVRHmdAng.x + m_MockVRHandPitchOffset, m_MockVRHmdAng.y, 0.0f };
        rightAng = QAngle{ m_MockVRHmdAng.x + m_MockVRHandPitchOffset, m_MockVRHmdAng.y, 0.0f };
    }

    // Velocity estimation (Source-local)
    const Vector hmdVel = (dt > 0.0f) ? (m_MockVRHmdPos - m_MockVRPrevHmdPos) * (1.0f / dt) : Vector{};
    const Vector leftVel = (dt > 0.0f) ? (leftPos - m_MockVRPrevLeftPos) * (1.0f / dt) : Vector{};
    const Vector rightVel = (dt > 0.0f) ? (rightPos - m_MockVRPrevRightPos) * (1.0f / dt) : Vector{};

    const QAngle hmdAngVel{ (dt > 0.0f) ? (MockDeltaDeg(m_MockVRHmdAng.x, m_MockVRPrevHmdAng.x) * (1.0f / dt)) : 0.0f,
                            (dt > 0.0f) ? (MockDeltaDeg(m_MockVRHmdAng.y, m_MockVRPrevHmdAng.y) * (1.0f / dt)) : 0.0f,
                            (dt > 0.0f) ? (MockDeltaDeg(m_MockVRHmdAng.z, m_MockVRPrevHmdAng.z) * (1.0f / dt)) : 0.0f };

    const QAngle leftAngVel{ (dt > 0.0f) ? (MockDeltaDeg(leftAng.x, m_MockVRPrevLeftAng.x) * (1.0f / dt)) : 0.0f,
                             (dt > 0.0f) ? (MockDeltaDeg(leftAng.y, m_MockVRPrevLeftAng.y) * (1.0f / dt)) : 0.0f,
                             (dt > 0.0f) ? (MockDeltaDeg(leftAng.z, m_MockVRPrevLeftAng.z) * (1.0f / dt)) : 0.0f };

    const QAngle rightAngVel{ (dt > 0.0f) ? (MockDeltaDeg(rightAng.x, m_MockVRPrevRightAng.x) * (1.0f / dt)) : 0.0f,
                              (dt > 0.0f) ? (MockDeltaDeg(rightAng.y, m_MockVRPrevRightAng.y) * (1.0f / dt)) : 0.0f,
                              (dt > 0.0f) ? (MockDeltaDeg(rightAng.z, m_MockVRPrevRightAng.z) * (1.0f / dt)) : 0.0f };

    // Update prev
    m_MockVRPrevHmdPos = m_MockVRHmdPos;
    m_MockVRPrevHmdAng = m_MockVRHmdAng;
    m_MockVRPrevLeftPos = leftPos;
    m_MockVRPrevLeftAng = leftAng;
    m_MockVRPrevRightPos = rightPos;
    m_MockVRPrevRightAng = rightAng;

    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses{};
    poses[vr::k_unTrackedDeviceIndex_Hmd] = MockMakePose(m_MockVRHmdPos, m_MockVRHmdAng, hmdVel, hmdAngVel);
    poses[1] = MockMakePose(leftPos, leftAng, leftVel, leftAngVel);
    poses[2] = MockMakePose(rightPos, rightAng, rightVel, rightAngVel);

    // Mark all other devices disconnected/invalid.
    for (uint32_t i = 3; i < vr::k_unMaxTrackedDeviceCount; ++i)
    {
        poses[i] = {};
        poses[i].bDeviceIsConnected = false;
        poses[i].bPoseIsValid = false;
        poses[i].eTrackingResult = vr::TrackingResult_Uninitialized;
    }

    // Publish (seqlock) for render-thread consumers.
    uint32_t seq = m_MockPoseSeq.load(std::memory_order_relaxed);
    m_MockPoseSeq.store(seq + 1, std::memory_order_release);
    m_MockPoseBuf = poses;
    m_MockPoseSeq.store(seq + 2, std::memory_order_release);

    // Also update the canonical pose array used by main-thread logic.
    std::memcpy(m_Poses, poses.data(), sizeof(m_Poses));
}

vr::EVRCompositorError VR::MockWaitGetPoses(vr::TrackedDevicePose_t* posesOut, uint32_t poseCount) const
{
    if (!posesOut || poseCount == 0)
        return vr::VRCompositorError_InvalidTexture;

    // Seqlock read.
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        const uint32_t s1 = m_MockPoseSeq.load(std::memory_order_acquire);
        if (s1 & 1u)
            continue;

        const uint32_t n = (poseCount < vr::k_unMaxTrackedDeviceCount) ? poseCount : vr::k_unMaxTrackedDeviceCount;
        for (uint32_t i = 0; i < n; ++i)
            posesOut[i] = m_MockPoseBuf[i];

        const uint32_t s2 = m_MockPoseSeq.load(std::memory_order_acquire);
        if (s1 == s2 && !(s2 & 1u))
            return vr::VRCompositorError_None;
    }

    // Fallback: best-effort copy.
    const uint32_t n = (poseCount < vr::k_unMaxTrackedDeviceCount) ? poseCount : vr::k_unMaxTrackedDeviceCount;
    for (uint32_t i = 0; i < n; ++i)
        posesOut[i] = m_MockPoseBuf[i];

    return vr::VRCompositorError_None;
}
