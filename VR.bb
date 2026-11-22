;----------------------------------------------------------------------------------------------------
; VR.bb - OpenXR support for SCP - Containment Breach
;
; Needs the BlitzX3D VR engine build (CameraProjOffAxis, SetRenderWorldBuffer, GraphicsD3D9Device,
; BufferD3D9Surface) and the BlitzXR userlib (BlitzXR.dll + openxr_loader.dll next to the exe,
; BlitzXR.decls in Userlibs\).
;
; Turn VR on with "-vr" on the command line or "enabled = 1" in the [vr] section of options.ini.
;
; How a frame works in VR:
;   VR_BeginFrame()   top of the main loop: waits for the headset, reads head + controller poses
;   MouseLook()       puts Camera at the player's head (VR_UpdateHeadCamera)
;   RenderWorld2()    RenderWorldMain() renders each eye into its own texture and hands it to OpenXR
;   VR_Flip()         ends the OpenXR frame. The 2D layer the game drew this frame (HUD, inventory,
;                     pause menu) goes over the 3D view: the HUD floats in front of your face, menus
;                     and inventory on a panel that stays where it opened. The main menu and loading
;                     screens have no 3D view and are shown on their own floating screen.
;
; Licensed CC BY-SA 3.0, same as the game.
;----------------------------------------------------------------------------------------------------

; controller buttons (BlitzXR button ids)
Const VRB_BLINK% = 0, VRB_SPRINT% = 1, VRB_CROUCH% = 2, VRB_INTERACT% = 3
Const VRB_INVENTORY% = 4, VRB_MENU% = 5, VRB_SAVE% = 6 ; Y saves (recentring is done by the VR software)

Global VRWanted% = (GetOptionInt("vr", "enabled") Lor HasCLIFlag("vr")) And (Not HasCLIFlag("novr"))
Global VREnabled% = False
Global VRInitError$ = ""

Global VRWorldScale# = 0.55   ; Blitz units per real metre (the player's eyes sit 0.9 units above the floor)
Global VRResScale# = 1.0
Global VRSnapTurn% = True
Global VRSnapAngle# = 30.0
Global VRSmoothTurnSpeed# = 2.5

; standing height: head height shortly after VR starts and after every recentre by the VR software
; (hold the Oculus / system button). The view treats it as the player's normal eye height.
Global VRStandHeadY# = 0.0
Global VRStandCaptureIn% = 30       ; frames until the standing height is (re)captured, -1 = captured

; real-life crouching: crouch in game when your head is this many metres below the standing height,
; stand again 0.1 m above that (so it doesn't flicker)
Global VRRealCrouchEnabled% = True
Global VRCrouchHeight# = 0.45
Global VRRealCrouch% = False        ; head is down right now
Global VRCrouchByHead% = False      ; Crouch was switched on by the head, not the crouch button
Global VRNoCrouchOffset% = False    ; the view follows the real head, so skip the game's crouch camera drop

; colour the 2D layer is cleared to in VR; must match BlitzXR.cpp (overlay key) and GFX\shaders\VRHudKey.fx
Const VR_KEY_R% = 0, VR_KEY_G% = 0, VR_KEY_B% = 12
Global VRKeyThisFrame% = False ; this frame's 2D layer is on the key colour (pause menu / inventory open)

; screens whose black parts are drawn solid instead of see-through: the pause menu and the inventories
Function VR_OpaqueUI%()
	Return MenuOpen Lor InvOpen Lor OtherOpen <> Null
End Function

Global VREyeW%, VREyeH%
Dim VREyeTex%(1)

;---------------------------------------------------------------------------------------------------- hand settings
; Tracked hands: GFX\VR\handleft.b3d / handright.b3d (skinned, modelled in metres), put on each controller's
; grip pose. Everything below is in the LEFT hand's terms; the right hand uses the mirror image (X offsets and
; yaw/roll negated), because the two models and the two grip frames are mirror images of each other.
;
; Grip frame (OpenXR /input/grip/pose, in Blitz axes): origin = centre of the controller handle inside your fist,
; +Z = out of the thumb side of the fist (along the handle), +X = out of the left palm (into the right palm),
; +Y = up from there (the wrist is roughly at -Y/-Z).
Global VRShowHands% = True
Global VRHandFileL$ = "GFX\VR\handleft.b3d"    ; swap these two if the hands come out mirrored (thumbs outward)
Global VRHandFileR$ = "GFX\VR\handright.b3d"
Global VRHandTexFile$ = "GFX\VR\T_VRHands_Base_color.jpg"
; Model -> grip transform, derived from each model's bind pose (vr-files\tools\derive_hands.py), [0] left [1] right:
;   pivot = the model-space point (metres) on the grip origin: the palm-surface centroid, 40% of the way from the
;           wrist (J_*_Hand) to the middle knuckle, moved out to the palm skin along the palm normal (OpenXR's
;           definition of the grip position)
;   rot   = RotateEntity pitch/yaw/roll mapping the model's palm normal to grip +X (left) / -X (right) and its
;           little->index knuckle line to grip +Z (Blitz; -Z in OpenXR), fingers then point to grip -Y
Dim VRHandPivot#(1, 2), VRHandBaseRot#(1, 2)
VRHandPivot(0, 0) = 0.0177 : VRHandPivot(0, 1) = -0.0130 : VRHandPivot(0, 2) = 0.0449
VRHandPivot(1, 0) = 0.0159 : VRHandPivot(1, 1) = -0.0089 : VRHandPivot(1, 2) = 0.0082
VRHandBaseRot(0, 0) = -69.27 : VRHandBaseRot(0, 1) = 47.81 : VRHandBaseRot(0, 2) = 157.20
VRHandBaseRot(1, 0) = -76.87 : VRHandBaseRot(1, 1) = -32.86 : VRHandBaseRot(1, 2) = -142.86
; adjustments on top, in grip space, all 0 by default ("hand offset x/y/z", "hand tilt", "hand pitch/yaw/roll" in
; [vr]). Given in the left hand's terms; the right hand gets the mirror image (X offset, yaw and roll negated).
Global VRHandPitch# = -15.0, VRHandYaw# = 0.0, VRHandRoll# = 0.0 ; pitch -15: tuned in the headset
; offset in metres: x = out of the left palm, y = up, z = toward the thumb / index side
Global VRHandOffX# = 0.0, VRHandOffY# = 0.0, VRHandOffZ# = 0.0
Global VRHandTilt# = 0.0
; finger curl: degrees each finger joint bends at full grip/trigger (joint 1/2/3 get 0.8/1.0/0.7 of it),
; how curled the fingers are with nothing pressed (0..1), and the bend direction (-1 if fingers bend backwards)
Global VRHandCurlMax# = 80.0
Global VRHandRestCurl# = 0.15
Global VRHandCurlSign# = 1.0
; "derived" = the fit above (default). "authored" = use the models exactly as placed in the .b3d files, with the
; file's origin on the grip point and its axes as the grip axes (for models you have posed around a controller yourself).
Global VRHandFitAuthored% = False
Global VRHandFullbright% = False   ; 1 = unaffected by the game's lighting (if the hands look too dark)

Global VRHandsLoaded% = False      ; tried to load for the current world (ClearWorld frees them: VR_ForgetHands)
Global VRHandRoot% = 0             ; pivot at the player's body origin, turned by VRBodyYaw (same as the camera)
Dim VRHandGrip%(1)                 ; follows the grip pose, child of VRHandRoot
Dim VRHandRot%(1)                  ; model rotation + nudges, child of VRHandGrip
Dim VRHandMesh%(1)                 ; the animated mesh, scaled to Blitz units, child of VRHandRot
Dim VRHandBone%(1, 14)             ; [hand, finger * 3 + joint]: fingers Thumb Index Middle Ring Pinky, joints 1-3
Dim VRHandBoneP#(1, 14), VRHandBoneY#(1, 14), VRHandBoneR#(1, 14) ; rest (bind) rotation of each bone
Dim VRHandCurl#(1, 1)              ; smoothed [hand, 0 = grip / 1 = trigger] 0..1
Dim VRHandTouch#(1, 1)             ; smoothed capacitive touch [hand, 0 = index on trigger / 1 = thumb on controls] 0..1
; physical grabbing (see the grabbing section)
Dim VRGrabObj%(1)               ; entity each hand holds (0 = nothing)
Dim VRGrabAngle#(1)             ; last hand angle: around a lever's hinge, or the wrist twist for 914
Dim VRGrabUseX%(1)              ; 914: twist measured with the palm normal (else the finger direction)
Dim VRGrabTick%(1)              ; 914: last knob position / key end stop, for haptic clicks
Dim VRGrabNearNow%(1), VRGrabNearLast%(1) ; what each hand can reach this / last frame (for the "in reach" tick)
; capacitive touch (Quest Touch, Index): with the finger off its control the index finger points / the thumb goes up;
; resting on it, the finger curls this much (0..1) before the trigger / grip adds the rest. Controllers without touch
; sensors (Vive, WMR) keep the pose above.
Global VRHandIndexTouchCurl# = 0.3
Global VRHandThumbTouchCurl# = 0.5
Global VRHandTouchTime# = 0.07     ; seconds a finger takes (about) to move when you touch / let go

Global VRBodyYaw# = 0.0          ; yaw of the player's body, changed only by turning (stick / mouse)
Global VRLastColliderYaw# = 0.0  ; what VR last set the collider to, to spot teleports by game code
Global VRSnapReady% = True
Global VRShouldRender% = False   ; XR_BeginFrame said the eyes should be drawn this frame
Global VRStereoThisFrame% = False

Function VR_Init()
	If Not VRWanted Then Return

	Local f# = GetOptionFloat("vr", "world scale") : If f > 0.0 Then VRWorldScale = f
	f = GetOptionFloat("vr", "resolution scale") : If f > 0.0 Then VRResScale = f
	If GetOptionString("vr", "snap turn") <> "" Then VRSnapTurn = GetOptionInt("vr", "snap turn")
	f = GetOptionFloat("vr", "snap angle") : If f > 0.0 Then VRSnapAngle = f
	f = GetOptionFloat("vr", "smooth turn speed") : If f > 0.0 Then VRSmoothTurnSpeed = f
	If GetOptionString("vr", "real crouch") <> "" Then VRRealCrouchEnabled = GetOptionInt("vr", "real crouch")
	f = GetOptionFloat("vr", "crouch height") : If f > 0.0 Then VRCrouchHeight = f
	If GetOptionString("vr", "show hands") <> "" Then VRShowHands = GetOptionInt("vr", "show hands")
	If GetOptionString("vr", "wrist hud") <> "" Then VRWristHUD = GetOptionInt("vr", "wrist hud")
	If GetOptionString("vr", "wrist hud offset x") <> "" Then VRWristOffX = GetOptionFloat("vr", "wrist hud offset x")
	If GetOptionString("vr", "wrist hud offset y") <> "" Then VRWristOffY = GetOptionFloat("vr", "wrist hud offset y")
	If GetOptionString("vr", "wrist hud offset z") <> "" Then VRWristOffZ = GetOptionFloat("vr", "wrist hud offset z")
	f = GetOptionFloat("vr", "wrist hud size") : If f > 0.0 Then VRWristSize = f
	If GetOptionString("vr", "hand fullbright") <> "" Then VRHandFullbright = GetOptionInt("vr", "hand fullbright")
	If GetOptionString("vr", "hand offset x") <> "" Then VRHandOffX = GetOptionFloat("vr", "hand offset x")
	If GetOptionString("vr", "hand offset y") <> "" Then VRHandOffY = GetOptionFloat("vr", "hand offset y")
	If GetOptionString("vr", "hand offset z") <> "" Then VRHandOffZ = GetOptionFloat("vr", "hand offset z")
	If GetOptionString("vr", "hand tilt") <> "" Then VRHandTilt = GetOptionFloat("vr", "hand tilt")
	If GetOptionString("vr", "hand pitch") <> "" Then VRHandPitch = GetOptionFloat("vr", "hand pitch")
	If GetOptionString("vr", "hand yaw") <> "" Then VRHandYaw = GetOptionFloat("vr", "hand yaw")
	If GetOptionString("vr", "hand roll") <> "" Then VRHandRoll = GetOptionFloat("vr", "hand roll")
	If Lower(GetOptionString("vr", "hand fit")) = "authored" Then VRHandFitAuthored = True
	If GetOptionString("vr", "physical grab") <> "" Then VRPhysicalGrab = GetOptionInt("vr", "physical grab")
	f = GetOptionFloat("vr", "grab radius") : If f > 0.0 Then VRGrabRadius = f
	If GetOptionString("vr", "physical keycards") <> "" Then VRPhysicalKeycards = GetOptionInt("vr", "physical keycards")
	If GetOptionString("vr", "held item flip") <> "" Then VRHeldFlip = GetOptionInt("vr", "held item flip")
	If GetOptionString("vr", "documents in hand") <> "" Then VRDocsInHand = GetOptionInt("vr", "documents in hand")
	f = GetOptionFloat("vr", "document size") : If f > 0.0 Then VRDocSize = f
	If GetOptionString("vr", "hand curl sign") <> "" Then VRHandCurlSign = GetOptionFloat("vr", "hand curl sign")
	If GetOptionString("vr", "hand index touch curl") <> "" Then VRHandIndexTouchCurl = GetOptionFloat("vr", "hand index touch curl")
	If GetOptionString("vr", "hand thumb touch curl") <> "" Then VRHandThumbTouchCurl = GetOptionFloat("vr", "hand thumb touch curl")

	If XR_Init(GraphicsD3D9Device(), "SCP - Containment Breach VR", VRResScale) Then
		VREnabled = True
		XR_SetWorldScale(VRWorldScale)
		XR_SetQuad(2.4, 2.0)
		f = GetOptionFloat("vr", "hud width") : If f <= 0.0 Then f = 1.3
		Local d# = GetOptionFloat("vr", "hud distance") : If d <= 0.0 Then d = 1.5
		XR_SetHud(f, d)
		VREyeW = XR_EyeWidth()
		VREyeH = XR_EyeHeight()
		Local i%
		For i = 0 To 1
			VREyeTex(i) = CreateTexture(VREyeW, VREyeH, 1 + 256)
		Next
		DebugLog "VR: " + XR_RuntimeName() + ", " + VREyeW + "x" + VREyeH + " per eye"
	Else
		VRInitError = XR_GetError()
		DebugLog "VR disabled: " + VRInitError
	EndIf
End Function

Function VR_Shutdown()
	If Not VREnabled Then Return
	Local i%
	For i = 0 To 1
		If VREyeTex(i) <> 0 Then FreeTexture VREyeTex(i) : VREyeTex(i) = 0
	Next
	XR_Shutdown()
	VREnabled = False
End Function

; Top of the main loop. Blocks until the headset wants the next frame.
Function VR_BeginFrame()
	VRStereoThisFrame = False
	If Not VREnabled Then Return
	VRShouldRender = XR_BeginFrame()
	If XR_ExitRequested() Then
		; the runtime closed our session (headset app quit or removed): carry on as a flat game
		VR_Shutdown()
	EndIf
End Function

; Replacement for Flip. In VR the headset paces the game, so the desktop never waits for vsync.
Function VR_Flip(vsync% = True)
	If VREnabled Then
		XR_BeginFrame() ; loading screens and menus call Flip without a VR_BeginFrame, this starts their frame
		If XR_FrameBegun() Then
			If VRStereoThisFrame Then
				; the back buffer holds only what the game drew in 2D this frame, on black
				If VRKeyThisFrame Then
					VR_UpdatePointer()
					XR_SubmitOverlay(BufferD3D9Surface(BackBuffer()), 3) ; panel, only the key colour see-through
				ElseIf VR_PanelOpen()
					VR_UpdatePointer()
					XR_SubmitOverlay(BufferD3D9Surface(BackBuffer()), 2)
				Else
					XR_SubmitOverlay(BufferD3D9Surface(BackBuffer()), 1)
				EndIf
				XR_EndFrame()
				VR_DrawDesktop()
			Else
				VR_UpdatePointer()
				XR_SubmitQuad(BufferD3D9Surface(BackBuffer()))
				XR_EndFrame()
			EndIf
			VRStereoThisFrame = False
			Flip False
			Return
		EndIf
	EndIf
	Flip vsync
End Function

; Stereo is used whenever there is a level loaded, including while paused or in the inventory.
Function VR_StereoActive%()
	If Not VREnabled Then Return False
	If Not VRShouldRender Then Return False
	If MainMenuOpen Then Return False
	Return True
End Function

; Screens that need the pointer: shown on a panel that stays in place instead of the head-locked HUD.
Function VR_PanelOpen%()
	If IsPaused() Then Return True
	If Using294 Then Return True
	If ConsoleOpen Then Return True
	Return False
End Function

; While paused the game skips MouseLook, but the headset still needs the camera to follow your head.
Function VR_UpdatePausedCamera()
	If Not VREnabled Then Return
	If MainMenuOpen Then Return
	If IsPaused() Lor EndingTimer < 0 Then VR_UpdateHeadCamera(False)
End Function

;---------------------------------------------------------------------------------------------------- input

; Laser pointer for the floating screen: aim a controller at it to move the mouse, right trigger clicks
; (the trigger is already mapped to mouse button 1 through VR_Hit/VR_Down(VRB_INTERACT)).
Global VRPointerLastX% = -1, VRPointerLastY% = -1
Global VRPointerSX# = -1.0, VRPointerSY# = -1.0

Function VR_UpdatePointer()
	If Not XR_PointerValid() Then
		VRPointerSX = -1.0
		Return
	EndIf
	Local tx# = XR_PointerX() * GraphicsWidth()
	Local ty# = XR_PointerY() * GraphicsHeight()
	; smooth out hand shake so buttons and double-clicks are easy to hit
	If VRPointerSX < 0.0 Then
		VRPointerSX = tx : VRPointerSY = ty
	Else
		VRPointerSX = VRPointerSX + (tx - VRPointerSX) * 0.45
		VRPointerSY = VRPointerSY + (ty - VRPointerSY) * 0.45
	EndIf
	Local x% = Int(VRPointerSX), y% = Int(VRPointerSY)
	; only move the mouse when the pointer moves, so a real mouse still works when the controller is still
	If Abs(x - VRPointerLastX) > 1 Lor Abs(y - VRPointerLastY) > 1 Then
		MoveMouse x, y
		VRPointerLastX = x : VRPointerLastY = y
	EndIf
	SetBuffer BackBuffer()
	If CursorIMG <> 0 Then
		; the game's own cursor (GFX\cursor.png, loaded in Main.bb), hotspot at its top-left like the desktop cursor
		DrawImage CursorIMG, MouseX(), MouseY()
	Else
		; before the cursor image is loaded (early loading screens)
		Color 0, 0, 0
		Oval MouseX() - 7, MouseY() - 7, 14, 14, True
		Color 255, 255, 255
		Oval MouseX() - 5, MouseY() - 5, 10, 10, True
	EndIf
End Function

Function VR_Down%(button%)
	If Not VREnabled Then Return False
	Return XR_ButtonDown(button)
End Function

Function VR_Hit%(button%)
	If Not VREnabled Then Return False
	Return XR_ButtonHit(button)
End Function

; left stick deflection, 0 inside the dead zone
Function VR_MoveMagnitude#()
	If Not VREnabled Then Return 0.0
	Local x# = XR_Axis(0), y# = XR_Axis(1)
	Local m# = Sqr(x * x + y * y)
	If m < 0.2 Then Return 0.0
	Return Min((m - 0.2) / 0.8, 1.0)
End Function

; movement angle in the same convention MovePlayer uses (0 = forward, 90 = left, -90 = right)
Function VR_MoveAngle#()
	Return -ATan2(XR_Axis(0), XR_Axis(1))
End Function

Function VR_HandleTurn()
	Local tx# = XR_Axis(2)
	If VRSnapTurn Then
		If Abs(tx) > 0.7 Then
			If VRSnapReady Then
				If tx > 0 Then VRBodyYaw = VRBodyYaw - VRSnapAngle Else VRBodyYaw = VRBodyYaw + VRSnapAngle
				VRSnapReady = False
			EndIf
		ElseIf Abs(tx) < 0.3
			VRSnapReady = True
		EndIf
	ElseIf Abs(tx) > 0.15
		VRBodyYaw = VRBodyYaw - tx * VRSmoothTurnSpeed * FPSfactor
	EndIf
	VRBodyYaw = WrapAngle(VRBodyYaw)
End Function

;---------------------------------------------------------------------------------------------------- head

; signed difference a - b in degrees, -180..180
Function VR_AngleDelta#(a#, b#)
	Local d# = WrapAngle(a - b)
	If d > 180.0 Then d = d - 360.0
	Return d
End Function

; Crouching in real life crouches in game (Crouch drives speed, noise and the game's crouch logic).
; The crouch button still toggles as before: the head only switches Crouch on when it goes down, and
; only switches it back off when it comes up if the head was what turned it on.
Function VR_UpdateRealCrouch()
	If Not VRRealCrouchEnabled Then
		VRRealCrouch = False : VRCrouchByHead = False : VRNoCrouchOffset = False
		Return
	EndIf
	If VRStandCaptureIn >= 0 Then Return ; standing height not captured yet
	Local drop# = (VRStandHeadY - XR_Head(1)) / VRWorldScale ; metres below the standing height
	If (Not VRRealCrouch) And drop > VRCrouchHeight Then
		VRRealCrouch = True
		If (Not Crouch) And (Not IsZombie) And Playable Then
			Crouch = True
			VRCrouchByHead = True
			VRNoCrouchOffset = True ; (if the button had already crouched you, the view keeps the game's drop: no jump)
		EndIf
	ElseIf VRRealCrouch And drop < VRCrouchHeight - 0.1 Then
		VRRealCrouch = False
		If VRCrouchByHead And Crouch Then Crouch = False
		VRCrouchByHead = False
	EndIf
	; keep following the real head until the game's crouch has fully eased back, so the view doesn't jump
	If VRNoCrouchOffset And (Not VRRealCrouch) And CrouchState < 0.01 Then VRNoCrouchOffset = False
End Function

; Body origin the camera and the hands share: collider + eye height (crouch included), turned by VRBodyYaw.
Global VRBodyX#, VRBodyY#, VRBodyZ#
Global VRCameraFollowsBody% = False ; VR_UpdateHeadCamera ran this frame (not during death / cutscene cameras)
Global VRCamSetX#, VRCamSetY#, VRCamSetZ# ; where VR_PlaceCamera last put the camera (to notice game code moving it)

; Camera at the head pose relative to the player's body, from the collider's current position.
Function VR_PlaceCamera()
	VRBodyX = EntityX(Collider, True)
	VRBodyY = EntityY(Collider, True) + VR_BodyEyeHeight() ; (crouched for real: the head pose already lowers the view)
	VRBodyZ = EntityZ(Collider, True)
	PositionEntity Camera, VRBodyX, VRBodyY, VRBodyZ, True
	RotateEntity Camera, 0, VRBodyYaw, 0, True
	MoveEntity Camera, XR_Head(0), XR_Head(1) - VRStandHeadY, XR_Head(2)
	TurnEntity Camera, XR_Head(3), XR_Head(4), XR_Head(5)
	user_camera_pitch = EntityPitch(Camera, True)
	VRCamSetX = EntityX(Camera, True) : VRCamSetY = EntityY(Camera, True) : VRCamSetZ = EntityZ(Camera, True)
End Function

; Called from MouseLook instead of the mouse-look code. Puts Camera where the player's head is.
Function VR_UpdateHeadCamera(handleInput%)
	; if game code turned the player (spawn, teleport, cutscene), turn the body with it
	Local ext# = VR_AngleDelta(EntityYaw(Collider), VRLastColliderYaw)
	If Abs(ext) > 0.01 Then VRBodyYaw = WrapAngle(VRBodyYaw + ext)

	; mouse still turns the body, which helps when testing at the desk
	Local mx# = MouseXSpeed() * (MouseSens + 0.6)
	MouseYSpeed()
	If handleInput And (Not IsNaN(mx)) Then
		VRBodyYaw = WrapAngle(VRBodyYaw - mx * mouselook_x_inc# / (1.0 + WearingVest))
		VR_HandleTurn()
	EndIf
	mouse_x_speed_1 = 0 : mouse_y_speed_1 = 0

	; re-capture the standing height a moment after the VR software recentres (the change takes effect a
	; little after it is announced) and once at startup, when the first head poses have arrived
	If XR_SpaceChanged() Then VRStandCaptureIn = 30
	If VRStandCaptureIn >= 0 Then
		VRStandCaptureIn = VRStandCaptureIn - 1
		If VRStandCaptureIn < 0 Then VRStandHeadY = XR_Head(1)
	EndIf

	If handleInput Then VR_UpdateRealCrouch()

	VR_PlaceCamera()
	VRCameraFollowsBody = True ; VR_RenderEyes places it again after the player has moved this frame

	; walking follows where you look
	RotateEntity Collider, EntityPitch(Collider), EntityYaw(Camera, True), EntityRoll(Collider)
	VRLastColliderYaw = EntityYaw(Collider)
End Function

;---------------------------------------------------------------------------------------------------- hands

; ClearWorld (NullGame) frees every entity and texture, so forget the old handles; they load again with the
; next level.
Function VR_ForgetHands()
	Local h%, b%
	VRHandsLoaded = False
	VRHandRoot = 0
	VRWristPivot = 0 : VRWristQuad = 0 : VRWristTex = 0 : VRWristShown = False : VRWristKey = -1
	VRGrabObj(0) = 0 : VRGrabObj(1) = 0 : VR914Held = False
	VRHeldPivot = 0 : VRHeldModel = 0 : VRHeldSrc = 0 : VRHeldLastButton = 0 ; (freed by ClearWorld)
	VRDocQuad = 0 : VRDocTex = 0 : VRDocImg = 0 : VRDocWanted = False
	For h = 0 To 1
		VRHandGrip(h) = 0 : VRHandRot(h) = 0 : VRHandMesh(h) = 0
		For b = 0 To 14 : VRHandBone(h, b) = 0 : Next
	Next
End Function

Function VR_LoadHands()
	VRHandsLoaded = True
	If Not VRShowHands Then Return

	Local tex% = 0
	If FileType(VRHandTexFile) = 1 Then tex = LoadTexture(VRHandTexFile, 1)

	VRHandRoot = CreatePivot()
	Local h%, f%, j%, side$, file$, finger$, bone%
	For h = 0 To 1
		If h = 0 Then file = VRHandFileL : side = "Left" Else file = VRHandFileR : side = "Right"
		If FileType(file) <> 1 Then
			DebugLog "VR hands: " + file + " not found"
		Else
			VRHandGrip(h) = CreatePivot(VRHandRoot)
			VRHandRot(h) = CreatePivot(VRHandGrip(h))
			VRHandMesh(h) = LoadAnimMesh(file, VRHandRot(h))
			If VRHandMesh(h) = 0 Then
				DebugLog "VR hands: couldn't load " + file
			Else
				If tex <> 0 Then EntityTexture VRHandMesh(h), tex
				If VRHandFullbright Then EntityFX VRHandMesh(h), 1
				EntityPickMode VRHandMesh(h), 0 ; never hit by the game's picks; no EntityType, so no collisions
				For f = 0 To 4
					Select f
						Case 0 : finger = "Thumb"
						Case 1 : finger = "Index"
						Case 2 : finger = "Middle"
						Case 3 : finger = "Ring"
						Default : finger = "Pinky"
					End Select
					For j = 0 To 2
						bone = FindChild(VRHandMesh(h), "J_" + side + "_Hand" + finger + (j + 1))
						VRHandBone(h, f * 3 + j) = bone
						If bone <> 0 Then
							VRHandBoneP(h, f * 3 + j) = EntityPitch(bone)
							VRHandBoneY(h, f * 3 + j) = EntityYaw(bone)
							VRHandBoneR(h, f * 3 + j) = EntityRoll(bone)
						EndIf
					Next
				Next
			EndIf
		EndIf
	Next
	; the mesh keeps its own reference to the texture
	If tex <> 0 Then FreeTexture tex
	VR_PlaceHandModels()
	If VRWristHUD And VRHandMesh(0) <> 0 Then VR_CreateWristHUD() : VR_PlaceWristHUD()
End Function

;---------------------------------------------------------------------------------------------------- wrist HUD

; Blink and stamina meters on a small screen floating just above the back of the left hand (like a watch: read it
; with the fingers to the right). The texture gets exactly what the 2D HUD draws (DrawHUDMetersAt in Main.bb, same
; images, same HUDScale), just moved to the texture's corner, and only when what it shows changes. Placed in grip
; space (see the hand settings), so it follows the hand and its adjustments.
Global VRWristHUD% = True
Global VRWristOffX# = 0.0, VRWristOffY# = 0.0, VRWristOffZ# = 0.0 ; "wrist hud offset x/y/z" (metres, grip space)
Global VRWristSize# = 1.0                                          ; "wrist hud size" (1 = 8 cm wide)
; where the screen sits by default, in grip space (metres): just off the back of the left hand (the palm is at x = 0
; and faces +X, the back of the hand is about 5 cm toward -X), halfway between wrist and knuckles
Const VR_WRIST_X# = -0.064, VR_WRIST_Y# = -0.008, VR_WRIST_Z# = 0.0
Const VR_WRIST_W# = 0.08 ; width in metres; the height follows the meters' shape

Global VRWristPivot% = 0, VRWristQuad% = 0, VRWristTex% = 0
Global VRWristW% = 0, VRWristH% = 0  ; pixel size of the meter block (at HUDScale), drawn at the texture's top left
Global VRWristShown% = False         ; the wrist screen is showing this frame, so DrawHUD leaves the meters out
Global VRWristKey% = -1              ; what the texture currently shows, to redraw only on change
Global VRWristMetersOnDesktop% = False ; DrawHUD left the meters out this frame: draw them on the desktop instead

Function VR_CreateWristHUD()
	If BlinkMeterIMG = 0 Lor StaminaMeterIMG = 0 Lor BlinkIcon = 0 Then Return ; HUD images not loaded yet

	; bounding box of what DrawHUDMetersAt draws, relative to its x, y: icons and their frames start 50 * HUDScale
	; left of the bars (the eye irritation frame 3 px further), the stamina row is 40 * HUDScale below
	Local spacing% = ImageWidth(BlinkMeterIMG) + 2
	Local barW% = Int(Int(204 * HUDScale) / spacing) * spacing + 3
	Local left% = Int(50 * HUDScale) + 4
	Local rowH% = Max(30 * HUDScale + 4, ImageHeight(StaminaMeterIMG) + 6)
	VRWristW = left + barW + 2
	VRWristH = Int(40 * HUDScale) + rowH + 4

	Local tw% = 64, th% = 32
	While tw < VRWristW : tw = tw * 2 : Wend
	While th < VRWristH : th = th * 2 : Wend
	VRWristTex = CreateTexture(tw, th, 1) ; managed: cheap to draw into with 2D commands
	If VRWristTex = 0 Then Return
	; the part of the texture the meters use (the texture may come out bigger than asked for)
	Local u1# = Float(VRWristW) / TextureWidth(VRWristTex), v1# = Float(VRWristH) / TextureHeight(VRWristTex)

	VRWristPivot = CreatePivot(VRHandGrip(0))
	VRWristQuad = CreateMesh(VRWristPivot)
	Local surf% = CreateSurface(VRWristQuad)
	Local s# = VRWorldScale
	Local hw# = VR_WRIST_W * 0.5 * VRWristSize * s
	Local hh# = hw * VRWristH / VRWristW
	Local cx# = (VR_WRIST_X + VRWristOffX) * s, cy# = (VR_WRIST_Y + VRWristOffY) * s, cz# = (VR_WRIST_Z + VRWristOffZ) * s
	; facing -X (out of the back of the left hand); text right = toward the fingers (-Y), text up = little finger side (-Z)
	AddVertex surf, cx, cy + hw, cz - hh, 0, 0   ; top left
	AddVertex surf, cx, cy - hw, cz - hh, u1, 0  ; top right
	AddVertex surf, cx, cy - hw, cz + hh, u1, v1 ; bottom right
	AddVertex surf, cx, cy + hw, cz + hh, 0, v1  ; bottom left
	AddTriangle surf, 0, 1, 2
	AddTriangle surf, 0, 2, 3
	EntityTexture VRWristQuad, VRWristTex
	EntityFX VRWristQuad, 1 + 8 + 16 ; fullbright, no fog, both sides (so the winding can't hide it)
	EntityBlend VRWristQuad, 3       ; additive: the black background is see-through
	EntityPickMode VRWristQuad, 0
	VRWristKey = -1
End Function

; Copy the user's hand adjustments (offset/rotation in grip space) so the wrist screen moves with the hand.
Function VR_PlaceWristHUD()
	If VRWristPivot = 0 Then Return
	Local s# = VRWorldScale
	PositionEntity VRWristPivot, VRHandOffX * s, VRHandOffY * s, VRHandOffZ * s
	RotateEntity VRWristPivot, VRHandTilt, 0, 0
	TurnEntity VRWristPivot, VRHandPitch, VRHandYaw, VRHandRoll
End Function

Function VR_UpdateWristHUD(leftTracked%)
	VRWristShown = False
	If VRWristQuad = 0 Then Return
	If (Not leftTracked) Lor (Not HUDenabled) Lor IsZombie Then
		HideEntity VRWristQuad
		Return
	EndIf
	ShowEntity VRWristQuad
	VRWristShown = True

	; redraw only when a bar gains or loses a block, or an icon changes
	Local spacing% = ImageWidth(BlinkMeterIMG) + 2
	Local blinkN% = Int(Max(0.0, BlinkTimer / BLINKFREQ) * 1000.0 / spacing)
	Local stamN% = Int(Max(0.0, Stamina / 100.0) * 1000.0 / spacing)
	Local key% = blinkN * 100000 + stamN * 100 + (EyeIrritation > 0) * 10 + Crouch
	If key = VRWristKey Then Return
	VRWristKey = key

	SetBuffer TextureBuffer(VRWristTex)
	ClsColor 0, 0, 0
	Cls
	DrawHUDMetersAt(Int(50 * HUDScale) + 4, 4) ; the same drawing as the 2D HUD, moved into the texture's corner
	SetBuffer BackBuffer()
End Function

; DrawHUD asks this: leave the blink/stamina meters out of the 2D HUD because they are on the hand
Function VR_WristMetersActive%()
	If Not VREnabled Then Return False
	If Not VRStereoThisFrame Then Return False
	Return VRWristShown
End Function

;---------------------------------------------------------------------------------------------------- desktop

; The desktop window gets the left eye with this frame's 2D layer (HUD, messages, menus, inventory) on top.
; The back buffer holds that layer on the key colour right after it went to the headset: copy it to the game's
; ScreenTexture (free at this point: the gamma pass that also uses it is skipped in VR), draw the eye, then draw the
; layer back over it with GFX\shaders\VRHudKey.fx, which leaves out only the key colour (black UI stays black).
Global VRHudKeyEffect% = 0, VRHudKeyTried% = False
Function VR_DrawDesktop()
	If (Not VRHudKeyTried) Then
		VRHudKeyTried = True
		If FileType("GFX\shaders\VRHudKey.fx") = 1 Then VRHudKeyEffect = LoadEffect("GFX\shaders\VRHudKey.fx")
		If VRHudKeyEffect = 0 Then DebugLog "VR: desktop HUD shader failed: " + GetEffectError()
	EndIf
	If ScreenTexture = 0 Lor PostEffectQuad = 0 Lor QuadCamera = 0 Lor (VRKeyThisFrame And VRHudKeyEffect = 0) Then
		VR_DrawMirror()
		Return
	EndIf
	CopyRect 0, 0, GraphicWidth, GraphicHeight, 0, 0, BackBuffer(), TextureBuffer(ScreenTexture)
	VR_DrawMirror()
	ShowEntity PostEffectQuad
	If VRKeyThisFrame Then
		SetQuadEffect(VRHudKeyEffect) ; pause menu / inventory: only the key colour left out
		EntityBlend PostEffectQuad, 0
	Else
		SetQuadEffect(0)              ; HUD and other screens: added on top, black is see-through
		EntityBlend PostEffectQuad, 3
	EndIf
	SetBuffer BackBuffer()
	CameraViewport QuadCamera, 0, 0, GraphicWidth, GraphicHeight
	RenderEntity PostEffectQuad, QuadCamera
	HideEntity PostEffectQuad
	EntityBlend PostEffectQuad, 0
	; the meters are on the hand in the headset; on the desktop draw them in their usual place
	If VRWristMetersOnDesktop Then DrawHUDMeters()
	VRWristMetersOnDesktop = False
End Function

; Model rotation, pivot and scale inside the grip frame (the settings at the top; call again after changing them).
Function VR_PlaceHandModels()
	Local h%, m#, s# = VRWorldScale
	For h = 0 To 1
		If VRHandMesh(h) <> 0 Then
			If h = 0 Then m = 1.0 Else m = -1.0 ; right hand: mirror image of the left
			PositionEntity VRHandRot(h), VRHandOffX * m * s, VRHandOffY * s, VRHandOffZ * s
			RotateEntity VRHandRot(h), VRHandTilt, 0, 0                              ; user adjustments (grip space)
			TurnEntity VRHandRot(h), VRHandPitch, VRHandYaw * m, VRHandRoll * m
			If Not VRHandFitAuthored Then TurnEntity VRHandRot(h), VRHandBaseRot(h, 0), VRHandBaseRot(h, 1), VRHandBaseRot(h, 2) ; derived
			ScaleEntity VRHandMesh(h), s, s, s
			If VRHandFitAuthored Then
				PositionEntity VRHandMesh(h), 0, 0, 0
			Else
				PositionEntity VRHandMesh(h), -VRHandPivot(h, 0) * s, -VRHandPivot(h, 1) * s, -VRHandPivot(h, 2) * s
			EndIf
		EndIf
	Next
End Function

; Height of the player's eyes above the collider before the head pose, same as VR_UpdateHeadCamera uses.
Function VR_BodyEyeHeight#()
	If VRNoCrouchOffset Then Return 0.6
	Return 0.6 + CrouchState * -0.3
End Function

; Every rendered VR frame: put the hands on the controllers and curl the fingers with grip / trigger.
Function VR_UpdateHands()
	VRWristShown = False
	If (Not VRHandsLoaded) And (Not MainMenuOpen) Then VR_LoadHands()
	If VRHandRoot = 0 Then Return
	VR_UpdateGrabFrame()
	If MainMenuOpen Lor (Not VRShowHands) Lor Collider = 0 Then
		HideEntity VRHandRoot
		Return
	EndIf
	ShowEntity VRHandRoot

	; the same origin the camera uses before the head offsets: collider + eye height, turned by the body yaw
	PositionEntity VRHandRoot, VRBodyX, VRBodyY, VRBodyZ, True
	RotateEntity VRHandRoot, 0, VRBodyYaw, 0, True

	Local h%, f%, j%, b%, c#, w#, target#, t%, touchEase#, rest#
	Local touch%[1]
	For h = 0 To 1
		If VRHandMesh(h) <> 0 Then
			If XR_GripValid(h) Then
				ShowEntity VRHandGrip(h)
				PositionEntity VRHandGrip(h), XR_Grip(h, 0), XR_Grip(h, 1) - VRStandHeadY, XR_Grip(h, 2)
				RotateEntity VRHandGrip(h), XR_Grip(h, 3), XR_Grip(h, 4), XR_Grip(h, 5)

				; smooth the analog values a little so the fingers don't twitch
				target = XR_GripValue(h)
				VRHandCurl(h, 0) = VRHandCurl(h, 0) + (target - VRHandCurl(h, 0)) * 0.5
				target = XR_TriggerValue(h)
				VRHandCurl(h, 1) = VRHandCurl(h, 1) + (target - VRHandCurl(h, 1)) * 0.5

				; capacitive touch, eased over about VRHandTouchTime (FPSfactor is 1 per 1/70 s)
				touchEase = 1.0 - Exp(-FPSfactor / (70.0 * Max(VRHandTouchTime, 0.001)))
				For t = 0 To 1
					touch[t] = XR_Touch(h, t)
					If touch[t] >= 0 Then VRHandTouch(h, t) = VRHandTouch(h, t) + (touch[t] - VRHandTouch(h, t)) * touchEase
				Next

				For f = 0 To 4
					Select f
						Case 0 ; thumb: a little with grip or trigger
							c = Max(VRHandCurl(h, 0), VRHandCurl(h, 1)) * 0.4
							If touch[1] < 0 Then
								c = VRHandRestCurl + (1.0 - VRHandRestCurl) * c
							Else ; off the controls: thumb up; resting on them: curled down onto them
								c = VRHandTouch(h, 1) * (VRHandThumbTouchCurl + (1.0 - VRHandThumbTouchCurl) * c)
							EndIf
						Case 1 ; index: trigger
							c = VRHandCurl(h, 1)
							If touch[0] < 0 Then
								c = VRHandRestCurl + (1.0 - VRHandRestCurl) * c
							Else ; off the trigger: pointing straight; resting on it: slightly curled, then the trigger adds the rest
								rest = VRHandTouch(h, 0) * VRHandIndexTouchCurl
								c = rest + (1.0 - rest) * c
							EndIf
						Default ; middle, ring, pinky: grip
							c = VRHandRestCurl + (1.0 - VRHandRestCurl) * VRHandCurl(h, 0)
					End Select
					If h = 1 And VRHeldModel <> 0 Then c = Max(c, 0.75) ; holding a keycard / severed hand
					For j = 0 To 2
						b = f * 3 + j
						If VRHandBone(h, b) <> 0 Then
							; the thumb's first joint swings sideways, so only bend its outer two
							If f = 0 And j = 0 Then
								w = 0.0
							Else
								Select j
									Case 0 : w = 0.8
									Case 1 : w = 1.0
									Default : w = 0.7
								End Select
							EndIf
							RotateEntity VRHandBone(h, b), VRHandBoneP(h, b), VRHandBoneY(h, b), VRHandBoneR(h, b)
							If w > 0.0 Then TurnEntity VRHandBone(h, b), c * w * VRHandCurlMax * VRHandCurlSign, 0, 0
						EndIf
					Next
				Next
			Else
				HideEntity VRHandGrip(h) ; controller not tracked
			EndIf
		EndIf
	Next
	VR_UpdateWristHUD(VRHandMesh(0) <> 0 And XR_GripValid(0))
	VR_UpdateDocSheet()
End Function

;---------------------------------------------------------------------------------------------------- grabbing

; Grab things with the controller grip, using the hand models' positions (so the hands must be shown):
;   levers (UpdateLever in MapSystem.bb): pull them around their hinge
;   SCP-914 key and knob (the "914" event in UpdateEvents.bb): twist your wrist to turn them
; The game's own code still decides what happens (on/off, sounds, 914 settings); this only moves the parts the way
; the mouse would. Haptics: a light tick when a hand comes in reach, a pulse on grabbing, clicks at lever and knob
; positions, a strong pulse at the lever's end stops and when the key starts 914.
Global VRPhysicalGrab% = True
Global VRGrabRadius# = 0.12     ; metres: how close the hand (palm) has to be to a lever handle or 914 part
Const VR_BTN_GRIP% = 7          ; BlitzXR button: 7 left grip, 8 right grip

Global VR914Held% = False       ; a hand is turning the 914 key or knob this frame
Global VR914Hand% = 0
Global VR914StateBefore# = 0

; levers and the 914 key/knob skip their hand / arrow icons when the hands can grab them
Function VR_GrabHidesIcons%()
	If Not (VREnabled And VRPhysicalGrab And VRShowHands) Then Return False
	Return VRHandMesh(0) <> 0 Lor VRHandMesh(1) <> 0
End Function

Function VR_HandCanGrab%(h%)
	If Not (VREnabled And VRPhysicalGrab) Then Return False
	If VRHandGrip(h) = 0 Then Return False
	Return XR_GripValid(h)
End Function

Function VR_ReleaseGrab(h%)
	If VRGrabObj(h) <> 0 And GrabbedEntity = VRGrabObj(h) Then GrabbedEntity = 0
	VRGrabObj(h) = 0
End Function

; once per rendered frame (from VR_UpdateHands): drop grabs whose grip was let go, and age the "in reach" state
Function VR_UpdateGrabFrame()
	Local h%
	For h = 0 To 1
		If VRGrabObj(h) <> 0 Then
			If (Not VR_HandCanGrab(h)) Lor (Not XR_ButtonDown(VR_BTN_GRIP + h)) Then VR_ReleaseGrab(h)
		EndIf
		VRGrabNearLast(h) = VRGrabNearNow(h)
		VRGrabNearNow(h) = 0
	Next
End Function

; hand h is in reach of obj: tick on coming in reach; grab it on a fresh grip press. True when just grabbed.
Function VR_TryGrab%(h%, obj%, radius#)
	If obj = 0 Lor VRGrabObj(h) <> 0 Lor VRGrabObj(1 - h) = obj Then Return False
	If Not VR_HandCanGrab(h) Then Return False
	If EntityDistance(VRHandGrip(h), obj) > radius Then Return False
	If VRGrabNearNow(h) = 0 Then
		VRGrabNearNow(h) = obj
		If VRGrabNearLast(h) <> obj Then XR_Haptic(h, 0.1, 10)
	EndIf
	If XR_ButtonHit(VR_BTN_GRIP + h) Then
		VRGrabObj(h) = obj
		XR_Haptic(h, 0.3, 20)
		Return True
	EndIf
	Return False
End Function

; which hand holds obj, -1 if none (and drops the grab if that hand let go)
Function VR_HandHolding%(obj%)
	Local h%
	For h = 0 To 1
		If VRGrabObj(h) = obj And obj <> 0 Then
			If VR_HandCanGrab(h) And XR_ButtonDown(VR_BTN_GRIP + h) Then Return h
			VR_ReleaseGrab(h)
		EndIf
	Next
	Return -1
End Function

;------------------------------------------------ levers

; Angle of the hand around a lever's hinge (the lever's local X axis, which RotateEntity pitch turns), in degrees,
; measured the same way as the lever's pitch (0 = up, + toward the lever's local +Z).
Function VR_LeverHandAngle#(h%, obj%)
	TFormPoint EntityX(VRHandGrip(h), True), EntityY(VRHandGrip(h), True), EntityZ(VRHandGrip(h), True), 0, GetParent(obj)
	Local x# = TFormedX() - EntityX(obj), y# = TFormedY() - EntityY(obj), z# = TFormedZ() - EntityZ(obj)
	Local yaw# = EntityYaw(obj)
	Return ATan2(-Sin(yaw) * x + Cos(yaw) * z, y)
End Function

; Called at the top of UpdateLever. Returns True while a hand holds the lever (UpdateLever then skips its mouse code).
Function VR_UpdateLeverGrab%(obj%, locked%)
	If Not VREnabled Then Return False
	Local h% = VR_HandHolding(obj)
	If h < 0 Then
		If locked Then Return False
		If EntityDistance(Camera, obj) > 1.5 Then Return False
		For h = 0 To 1
			; the game picks levers with a 0.1 radius sphere at the hinge, so reach is measured to that
			If VR_TryGrab(h, obj, 0.1 + VRGrabRadius * VRWorldScale) Then
				VRGrabAngle(h) = VR_LeverHandAngle(h, obj)
				Exit
			EndIf
		Next
		If h > 1 Then Return False
	ElseIf locked
		VR_ReleaseGrab(h)
		Return False
	EndIf

	; turn the lever by however far the hand went around the hinge since last frame, within the game's limits
	Local a# = VR_LeverHandAngle(h, obj)
	Local d# = VR_AngleDelta(a, VRGrabAngle(h))
	VRGrabAngle(h) = a
	Local prev# = EntityPitch(obj)
	Local p# = Max(Min(prev + d, 80.0), -80.0)
	RotateEntity obj, p, EntityYaw(obj), 0

	; same clunk as the mouse code at the end stops
	If p > 75 And prev <= 75 Then PlaySound2(LeverSFX, Camera, obj, 1.0) : XR_Haptic(h, 0.8, 40)
	If p < -75 And prev >= -75 Then PlaySound2(LeverSFX, Camera, obj, 1.0) : XR_Haptic(h, 0.8, 40)
	If (p > 0) <> (prev > 0) Then XR_Haptic(h, 0.5, 15) ; past the middle: the lever's state flips
	Return True
End Function

;------------------------------------------------ SCP-914

; wrist twist around a 914 part's turning axis (its parent's Z axis: the event only ever sets its roll)
Function VR_TwistAngle#(h%, obj%)
	If VRGrabUseX(h) Then
		TFormVector 1, 0, 0, VRHandGrip(h), GetParent(obj)
	Else
		TFormVector 0, 1, 0, VRHandGrip(h), GetParent(obj)
	EndIf
	Return ATan2(TFormedY(), TFormedX())
End Function

Function VR_StartTwist(h%, obj%)
	; measure the twist with whichever hand axis lies flattest across the turning axis
	TFormVector 1, 0, 0, VRHandGrip(h), GetParent(obj)
	Local lx# = TFormedX() * TFormedX() + TFormedY() * TFormedY()
	TFormVector 0, 1, 0, VRHandGrip(h), GetParent(obj)
	Local ly# = TFormedX() * TFormedX() + TFormedY() * TFormedY()
	VRGrabUseX(h) = (lx >= ly)
	VRGrabAngle(h) = VR_TwistAngle(h, obj)
	VRGrabTick(h) = VR_914KnobPos(obj)
End Function

; the event's knob positions: 2 rough, 1 coarse, 0 1:1, -1 fine, -2 very fine
Function VR_914KnobPos%(knob%)
	Local angle# = WrapAngle(EntityRoll(knob))
	If angle < 22.5 Then Return 0
	If angle < 67.5 Then Return 1
	If angle < 180 Then Return 2
	If angle > 337.5 Then Return 0
	If angle > 292.5 Then Return -1
	Return -2
End Function

; Called in the 914 event right before its mouse handling. While a hand holds the key or knob, it becomes the
; GrabbedEntity and the wrist twist is fed in as mouse movement, so the event's own turning code, limits and
; start check do the rest (the event condition also checks VR914Held).
Function VR_Update914(key%, knob%, state#)
	VR914Held = False
	VR914StateBefore = state
	If Not VREnabled Then Return
	Local h%, obj%
	For h = 0 To 1
		obj = VRGrabObj(h)
		If obj <> 0 And (obj = key Lor obj = knob) Then
			If VR_HandHolding(obj) = h Then
				Local a# = VR_TwistAngle(h, obj)
				Local d# = VR_AngleDelta(a, VRGrabAngle(h))
				VRGrabAngle(h) = a
				GrabbedEntity = obj
				mouse_x_speed_1 = -d / 2.5 ; the event turns the grabbed part by -mouse_x_speed_1 * 2.5
				VR914Held = True
				VR914Hand = h
				Return
			EndIf
		EndIf
	Next
	If state <> 0 Then Return ; 914 is running: the key and knob can't move
	If EntityDistance(Camera, knob) > 1.5 Then Return
	For h = 0 To 1
		If VR_TryGrab(h, key, 0.05 + VRGrabRadius * VRWorldScale) Then VR_StartTwist(h, key) : Return
		If VR_TryGrab(h, knob, 0.05 + VRGrabRadius * VRWorldScale) Then VR_StartTwist(h, knob) : Return
	Next
End Function

; Called right after the event's mouse handling: haptic clicks, and clear the fake mouse movement.
Function VR_After914(key%, knob%, state#)
	If Not VR914Held Then Return
	mouse_x_speed_1 = 0
	Local h% = VR914Hand, pos%
	If VRGrabObj(h) = knob Then
		pos = VR_914KnobPos(knob)
		If pos <> VRGrabTick(h) Then XR_Haptic(h, 0.5, 15) : VRGrabTick(h) = pos
	ElseIf VRGrabObj(h) = key
		; the key stops at 180 degrees; turning it there with something in the intake starts 914
		pos = (Abs(WrapAngle(EntityRoll(key)) - 180.0) < 1.0)
		If pos And (Not VRGrabTick(h)) Then XR_Haptic(h, 0.6, 25)
		VRGrabTick(h) = pos
		If VR914StateBefore = 0 And state <> 0 Then XR_Haptic(h, 0.8, 40)
	EndIf
End Function

;---------------------------------------------------------------------------------------------------- held keycards

; With a keycard (key1-key6, keyomni) or a severed hand (hand, hand2) selected in the inventory, the right hand holds
; that item's own 3D model, sticking forward out of the fist. Touch its front end to a door's keycard reader or hand
; scanner and the door is used exactly as if you had clicked that button with the item selected (UseDoor decides:
; access level, messages, sounds, the item going back to the inventory). Clicking with the trigger still works.
Global VRPhysicalKeycards% = True
Global VRHeldFlip% = False          ; "held item flip": turn the held item around (if the severed hand is held by its fingers)
Global VRHeldTouchRadius# = 0.06    ; metres between the item's front end and a reader that count as touching
Global VRHeldSrc% = 0               ; the selected item's model the held copy was made from (0 = holding nothing)
Global VRHeldPivot% = 0, VRHeldModel% = 0
Global VRHeldHalf# = 0              ; half the held item's length, Blitz units
Global VRHeldLastButton% = 0        ; the reader last used, until the item moves away from it (one touch = one use)
Global VRHeldPulseAt% = 0           ; MilliSecs() of the second "access granted" pulse, 0 = none

Function VR_HeldKind%(name$)
	Select name
		Case "key1", "key2", "key3", "key4", "key5", "key6", "keyomni" : Return 1
		Case "hand", "hand2" : Return 2
	End Select
	Return 0
End Function

Function VR_DropHeld()
	If VRHeldPivot <> 0 Then FreeEntity VRHeldPivot
	VRHeldPivot = 0 : VRHeldModel = 0 : VRHeldSrc = 0 : VRHeldLastButton = 0
End Function

; copy the item's world model into the right hand, its longest side pointing forward out of the fist (grip +Z) and
; its thinnest side across the palm (grip X), so a card is held upright between thumb and fingers
Function VR_MakeHeld(src%)
	VRHeldSrc = src
	VRHeldPivot = CreatePivot(VRHandGrip(1))
	VRHeldModel = CopyEntity(src, VRHeldPivot)
	ShowEntity VRHeldModel
	EntityPickMode VRHeldModel, 0
	EntityType VRHeldModel, 0
	RotateEntity VRHeldModel, 0, 0, 0
	PositionEntity VRHeldModel, 0, 0, 0

	; bounds of the mesh in its own units
	Local minX# = 1000000, minY# = 1000000, minZ# = 1000000, maxX# = -1000000, maxY# = -1000000, maxZ# = -1000000
	Local s%, v%, surf%, x#, y#, z#
	For s = 1 To CountSurfaces(VRHeldModel)
		surf = GetSurface(VRHeldModel, s)
		For v = 0 To CountVertices(surf) - 1
			x = VertexX(surf, v) : y = VertexY(surf, v) : z = VertexZ(surf, v)
			If x < minX Then minX = x
			If y < minY Then minY = y
			If z < minZ Then minZ = z
			If x > maxX Then maxX = x
			If y > maxY Then maxY = y
			If z > maxZ Then maxZ = z
		Next
	Next
	If minX > maxX Then minX = 0 : maxX = 0 : minY = 0 : maxY = 0 : minZ = 0 : maxZ = 0
	Local ex# = (maxX - minX) * EntityScaleX(VRHeldModel, True)
	Local ey# = (maxY - minY) * EntityScaleY(VRHeldModel, True)
	Local ez# = (maxZ - minZ) * EntityScaleZ(VRHeldModel, True)
	Local longAxis% = 1, thinAxis% = 1
	If ey > ex And ey >= ez Then longAxis = 2
	If ez > ex And ez > ey Then longAxis = 3
	If ey < ex And ey <= ez Then thinAxis = 2
	If ez < ex And ez < ey Then thinAxis = 3
	If thinAxis = longAxis Then thinAxis = (longAxis Mod 3) + 1
	Select longAxis
		Case 1 : VRHeldHalf = ex * 0.5
		Case 2 : VRHeldHalf = ey * 0.5
		Default : VRHeldHalf = ez * 0.5
	End Select

	; hold point in grip space (right hand: the palm faces -X, the thumb side of the fist is +Z): between thumb and
	; index finger. The item's centre sits a bit less than half its length further forward.
	Local m# = VRWorldScale
	PositionEntity VRHeldPivot, -0.02 * m, 0.0, 0.03 * m
	RotateEntity VRHeldPivot, 0, 0, 0

	Local f# = 1.0
	If VRHeldFlip Then f = -1.0
	TFormVector 0, 0, f, VRHeldPivot, 0
	AlignToVector VRHeldModel, TFormedX(), TFormedY(), TFormedZ(), longAxis, 1.0
	TFormVector 1, 0, 0, VRHeldPivot, 0
	AlignToVector VRHeldModel, TFormedX(), TFormedY(), TFormedZ(), thinAxis, 1.0

	; move the mesh so its centre is on the pivot's +Z axis, 0.8 of its half length out
	TFormPoint (minX + maxX) * 0.5, (minY + maxY) * 0.5, (minZ + maxZ) * 0.5, VRHeldModel, VRHeldPivot
	PositionEntity VRHeldModel, -TFormedX(), -TFormedY(), -TFormedZ() + VRHeldHalf * 0.8
End Function

; Called at the end of UpdateDoors (Main.bb), once per game frame.
Function VR_UpdateHeldItem()
	If VRHeldPulseAt <> 0 And MilliSecs() >= VRHeldPulseAt Then XR_Haptic(1, 0.6, 40) : VRHeldPulseAt = 0

	Local want% = 0
	If VREnabled And VRPhysicalKeycards And VRShowHands And VRHandMesh(1) <> 0 And SelectedItem <> Null Then
		If (Not InvOpen) And OtherOpen = Null And (Not MenuOpen) Then
			If VR_HeldKind(SelectedItem\itemtemplate\name) <> 0 Then want = SelectedItem\model
		EndIf
	EndIf
	If want <> VRHeldSrc Then
		VR_DropHeld()
		If want <> 0 Then VR_MakeHeld(want)
	EndIf
	If VRHeldModel = 0 Lor (Not XR_GripValid(1)) Lor (Not Playable) Then Return

	; front end (and centre) of the held item in the world
	TFormPoint 0, 0, VRHeldHalf * 1.8, VRHeldPivot, 0
	Local tx# = TFormedX(), ty# = TFormedY(), tz# = TFormedZ()
	TFormPoint 0, 0, VRHeldHalf * 0.8, VRHeldPivot, 0
	Local cx# = TFormedX(), cy# = TFormedY(), cz# = TFormedZ()
	Local r# = VRHeldTouchRadius * VRWorldScale + 0.02 ; (+ about half a reader's size)

	Local d.Doors, i%, b%, dist#, bx#, by#, bz#
	; re-arm once the item has moved away from the reader it last used
	If VRHeldLastButton <> 0 Then
		bx = EntityX(VRHeldLastButton, True) : by = EntityY(VRHeldLastButton, True) : bz = EntityZ(VRHeldLastButton, True)
		dist = Min(Sqr((tx-bx)*(tx-bx) + (ty-by)*(ty-by) + (tz-bz)*(tz-bz)), Sqr((cx-bx)*(cx-bx) + (cy-by)*(cy-by) + (cz-bz)*(cz-bz)))
		If dist > r + 0.1 * VRWorldScale Then VRHeldLastButton = 0
	EndIf

	For d = Each Doors
		If d\KeyCard <> 0 And d\dist < 3.0 And (d\openstate >= 180 Lor d\openstate <= 0) Then
			For i = 0 To 1
				b = d\buttons[i]
				If b <> 0 And b <> VRHeldLastButton Then
					bx = EntityX(b, True) : by = EntityY(b, True) : bz = EntityZ(b, True)
					dist = Min(Sqr((tx-bx)*(tx-bx) + (ty-by)*(ty-by) + (tz-bz)*(tz-bz)), Sqr((cx-bx)*(cx-bx) + (cy-by)*(cy-by) + (cz-bz)*(cz-bz)))
					If dist < r Then
						; the same as clicking the reader with the item selected (DrawGUI's MouseUp1 path)
						VRHeldLastButton = b
						Local wasOpen% = d\open
						XR_Haptic(1, 0.5, 30)
						PlaySound2(ButtonSFX, Camera, b)
						UseDoor(d, True)
						If d\open <> wasOpen Then
							VRHeldPulseAt = MilliSecs() + 110 ; access granted: a second pulse
						Else
							XR_Haptic(1, 1.0, 120)            ; denied: one long buzz
						EndIf
						Return
					EndIf
				EndIf
			Next
		EndIf
	Next
End Function

;---------------------------------------------------------------------------------------------------- documents in hand

; With a document selected (papers, notes, badges, SCP-1025, the ticket), the page the game would draw full screen
; goes onto a sheet held in the left hand instead, facing you, to the right of the hand (so the right hand stays
; free). The game's own image is used, including notes it writes codes onto. No left hand: the normal 2D view.
Global VRDocsInHand% = True
Global VRDocSize# = 1.0             ; "document size": 1 = 29.7 cm tall (A4), width from the page's own shape
Global VRDocImg% = 0                ; the image the sheet currently shows
Global VRDocQuad% = 0, VRDocTex% = 0
Global VRDocWanted% = False         ; DrawGUI asked for the sheet since the last rendered frame

; Called by DrawGUI where it would draw a selected document full screen. True = the page is on the hand this frame,
; so the 2D page is left out.
Function VR_DocumentInHand%(img%)
	If img = 0 Then Return False
	If Not (VREnabled And VRDocsInHand And VRShowHands) Then Return False
	If VRHandMesh(0) = 0 Lor VRHandGrip(0) = 0 Then Return False
	If Not XR_GripValid(0) Then Return False
	If img <> VRDocImg Lor VRDocQuad = 0 Then VR_MakeDocSheet(img)
	If VRDocQuad = 0 Then Return False
	VRDocWanted = True
	Return True
End Function

Function VR_FreeDocSheet()
	If VRDocQuad <> 0 Then FreeEntity VRDocQuad
	If VRDocTex <> 0 Then FreeTexture VRDocTex
	VRDocQuad = 0 : VRDocTex = 0 : VRDocImg = 0
End Function

Function VR_MakeDocSheet(img%)
	VR_FreeDocSheet()
	Local w% = ImageWidth(img), h% = ImageHeight(img)
	If w <= 0 Lor h <= 0 Then Return
	Local tw% = 64, th% = 64
	While tw < w : tw = tw * 2 : Wend
	While th < h : th = th * 2 : Wend
	VRDocTex = CreateTexture(tw, th, 1)
	If VRDocTex = 0 Then Return
	CopyRect 0, 0, w, h, 0, 0, ImageBuffer(img), TextureBuffer(VRDocTex)
	Local u1# = Float(w) / TextureWidth(VRDocTex), v1# = Float(h) / TextureHeight(VRDocTex)

	; grip space of the left hand: the palm faces +X, the thumb side of the fist is +Z (forward), the fingers -Y.
	; The sheet faces back toward you (-Z), held by its left edge just in front of the thumb, reaching to the right.
	Local s# = VRWorldScale
	Local sh# = 0.297 * VRDocSize * s, sw# = sh * w / h
	Local x0# = 0.01 * s, yb# = -0.06 * s, z# = 0.04 * s
	VRDocQuad = CreateMesh(VRHandGrip(0))
	Local surf% = CreateSurface(VRDocQuad)
	AddVertex surf, x0, yb + sh, z, 0, 0       ; top left
	AddVertex surf, x0 + sw, yb + sh, z, u1, 0 ; top right
	AddVertex surf, x0 + sw, yb, z, u1, v1     ; bottom right
	AddVertex surf, x0, yb, z, 0, v1           ; bottom left
	AddTriangle surf, 0, 1, 2
	AddTriangle surf, 0, 2, 3
	EntityTexture VRDocQuad, VRDocTex
	EntityFX VRDocQuad, 1 + 8 + 16 ; fullbright, no fog, both sides
	EntityPickMode VRDocQuad, 0
	HideEntity VRDocQuad
	VRDocImg = img
End Function

; every rendered frame (VR_UpdateHands): show the sheet only while DrawGUI keeps asking for it
Function VR_UpdateDocSheet()
	If VRDocQuad = 0 Then Return
	If VRDocWanted And XR_GripValid(0) Then ShowEntity VRDocQuad Else HideEntity VRDocQuad
	VRDocWanted = False
End Function

;---------------------------------------------------------------------------------------------------- rendering

; Used by RenderWorld2 in place of RenderWorld for the main 3D pass.
Function RenderWorldMain()
	If Not VR_RenderEyes() Then RenderWorld()
End Function

Function VR_RenderEyes%()
	If Not VR_StereoActive() Then Return False

	; MouseLook placed the camera before the player moved this frame: place it (and the hands, from the same
	; origin) again from where the collider is now, so neither lags a frame behind while walking
	If VRCameraFollowsBody And Abs(EntityX(Camera, True) - VRCamSetX) + Abs(EntityY(Camera, True) - VRCamSetY) + Abs(EntityZ(Camera, True) - VRCamSetZ) < 0.0001 Then
		VR_PlaceCamera() ; (skipped if game code moved the camera since MouseLook, e.g. a scripted scene)
		VRCameraFollowsBody = False
	ElseIf Collider <> 0
		VRCameraFollowsBody = False
		VRBodyX = EntityX(Collider, True) : VRBodyY = EntityY(Collider, True) + VR_BodyEyeHeight() : VRBodyZ = EntityZ(Collider, True)
	EndIf
	VR_UpdateHands()

	Local cx# = EntityX(Camera, True), cy# = EntityY(Camera, True), cz# = EntityZ(Camera, True)
	Local cp# = EntityPitch(Camera, True), cyw# = EntityYaw(Camera, True), cr# = EntityRoll(Camera, True)
	Local eye%

	For eye = 0 To 1
		PositionEntity Camera, cx, cy, cz, True
		RotateEntity Camera, cp, cyw, cr, True
		MoveEntity Camera, XR_EyeOffset(eye, 0), XR_EyeOffset(eye, 1), XR_EyeOffset(eye, 2)
		TurnEntity Camera, XR_EyeOffset(eye, 3), XR_EyeOffset(eye, 4), XR_EyeOffset(eye, 5)
		CameraProjOffAxis Camera, XR_EyeFov(eye, 0), XR_EyeFov(eye, 1), XR_EyeFov(eye, 2), XR_EyeFov(eye, 3)
		CameraViewport Camera, 0, 0, VREyeW, VREyeH

		SetRenderWorldBuffer TextureBuffer(VREyeTex(eye))
		RenderWorld()

		; full-view overlays (blink darkness, gas mask, night vision tint) from the overlay camera
		If ark_blur_cam <> 0 Then
			CameraProjMode Camera, 0
			CameraProjMode ark_blur_cam, 2
			CameraViewport ark_blur_cam, 0, 0, VREyeW, VREyeH
			HideEntity ark_blur_image ; desktop motion blur, not used in VR
			RenderWorld()
			ShowEntity ark_blur_image
			CameraProjMode ark_blur_cam, 0
			CameraProjMode Camera, 1
		EndIf

		SetRenderWorldBuffer 0
		XR_SubmitEye(eye, BufferD3D9Surface(TextureBuffer(VREyeTex(eye))))
	Next

	; back to the desktop camera setup
	PositionEntity Camera, cx, cy, cz, True
	RotateEntity Camera, cp, cyw, cr, True
	CameraProjOffAxis Camera, 0, 0, 0, 0
	CameraViewport Camera, 0, 0, GraphicWidth, GraphicHeight
	If ark_blur_cam <> 0 Then CameraViewport ark_blur_cam, 0, 0, ark_sw, ark_sh

	; the desktop mirror is drawn in VR_Flip, after the 2D layer has gone to the headset. The 2D layer goes on black
	; (black is see-through), except with the pause menu or inventory open: then it goes on the key colour, which
	; BlitzXR (headset) and VRHudKey.fx (desktop) leave out instead, so the menu's black parts stay black.
	SetBuffer BackBuffer()
	VRKeyThisFrame = VR_OpaqueUI()
	If VRKeyThisFrame Then ClsColor VR_KEY_R, VR_KEY_G, VR_KEY_B
	Cls
	ClsColor 0, 0, 0
	VRStereoThisFrame = True
	Return True
End Function

; Desktop window: the left eye, cropped to the window's shape. (The HUD is only in the headset.)
Function VR_DrawMirror()
	If VREyeTex(0) = 0 Then Return
	Local sw% = VREyeW, sh% = VREyeH, sx% = 0, sy% = 0
	If Float(VREyeW) / VREyeH < Float(GraphicWidth) / GraphicHeight Then
		sh = VREyeW * GraphicHeight / GraphicWidth : sy = (VREyeH - sh) / 2
	Else
		sw = VREyeH * GraphicWidth / GraphicHeight : sx = (VREyeW - sw) / 2
	EndIf
	CopyRectStretch sx, sy, sw, sh, 0, 0, GraphicWidth, GraphicHeight, TextureBuffer(VREyeTex(0)), BackBuffer()
End Function

;~IDEal Editor Parameters:
;~C#Blitz3D
