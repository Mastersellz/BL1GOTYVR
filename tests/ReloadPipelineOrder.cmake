# Source contracts from the headset-validated original DLL. These checks do
# NOT observe native animation execution, skinning/upload or GPU visibility.
file(READ "${SOURCE_DIR}/src/input/InputHook.cpp" input)
foreach(signature IN ITEMS "ReapplyWeaponPose(void* component)" "ReapplyWeaponPose()")
    string(FIND "${input}" "bool InputHook::${signature}" start)
    if(start LESS 0)
        message(FATAL_ERROR "Missing weapon reapplication entry point")
    endif()
    string(SUBSTRING "${input}" ${start} -1 tail)
    string(FIND "${tail}" "\n}\n" end)
    string(SUBSTRING "${tail}" 0 ${end} body)
    string(FIND "${body}" "ApplyMagBoneOverride();" magazine)
    string(FIND "${body}" "if (" first_guard)
    string(FIND "${body}" "written = WriteProcessMemory(" stamp)
    if(magazine LESS 0 OR first_guard LESS magazine OR stamp LESS magazine)
        message(FATAL_ERROR "Original reload runs unconditionally BEFORE component filtering and weapon stamping")
    endif()
endforeach()
foreach(required IN ITEMS
    "MagazinePoseScope poseScope(m_magPoseLock)"
    "NeedsOriginalMagazineCapture(mode, m_magHold.grabRotationValid)"
    "GetLeftGripCenterWorld(gripWorld)"
    "CanAcquireMagazine(gripPressed, dWell, kWellExtract)"
    "CanAcquireMagazine(gripPressed, dPouch, kPouchExtract)"
    "distance3(offHand.aimPosition, well)"
    "memcpy(seq.wellAnchor, well, sizeof(seq.wellAnchor))"
    "else if (wellFromBone) well[1] -= 0.08f"
    "desiredWorld[2] += 3.0f"
    "InvertRigidFrame(componentMatrix, inverseComponent)")
    string(FIND "${input}" "${required}" found)
    if(found LESS 0)
        message(FATAL_ERROR "Original reload contract missing: ${required}")
    endif()
endforeach()
foreach(forbidden IN ITEMS
    "m_weaponPoseThreadId" "PublishMagHold(4)" "RefreshSeatedMagazine("
    "GetSeatedMagazineStagePosition(" "GetLeftGripCenterWorld(desiredWorld, handRotation)")
    string(FIND "${input}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "Failed experimental reload path reintroduced: ${forbidden}")
    endif()
endforeach()
string(FIND "${input}" "bool InputHook::ReadMagazinePose(" start)
string(SUBSTRING "${input}" ${start} -1 tail)
string(FIND "${tail}" "\n}\n" end)
string(SUBSTRING "${tail}" 0 ${end} reader)
string(FIND "${reader}" "view.key.component + view.matrixOffset), view.componentMatrix" native_matrix)
string(FIND "${reader}" "memcpy(view.componentMatrix, m_renderWeaponMatrix" cached_matrix)
string(FIND "${reader}" "active = m_renderWeaponStampActive" stamp_gated)
string(FIND "${reader}" "active = m_weaponPoseActive.load" pose_valid)
if(native_matrix LESS 0 OR NOT cached_matrix EQUAL -1 OR NOT stamp_gated EQUAL -1 OR pose_valid LESS 0)
    message(FATAL_ERROR "Original reload must read the native matrix resident at this callback")
endif()
file(READ "${SOURCE_DIR}/src/xr/FrameLoop.cpp" frames)
string(FIND "${frames}" "tanf(eyeView.fov.angleLeft), tanf(eyeView.fov.angleRight)" original_projection)
string(FIND "${frames}" "ReloadProjection cueProjection" experimental_projection)
if(original_projection LESS 0 OR NOT experimental_projection EQUAL -1)
    message(FATAL_ERROR "Original indicator projection must not depend on the experimental source-crop helper")
endif()
message(STATUS "Original v0.5.7.3 reload compatibility contracts passed")
