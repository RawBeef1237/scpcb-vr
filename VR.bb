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
Const VRB_INVENTORY% = 4, VRB_MENU% = 5, VRB_RECENTER% = 6

Global VRWanted% = (GetOptionInt("vr", "enabled") Lor HasCLIFlag("vr")) And (Not HasCLIFlag("novr"))
Global VREnabled% = False
Global VRInitError$ = ""

Global VRWorldScale# = 0.55   ; Blitz units per real metre (the player's eyes sit 0.9 units above the floor)
Global VRResScale# = 1.0
Global VRSnapTurn% = True
Global VRSnapAngle# = 30.0
Global VRSmoothTurnSpeed# = 2.5

; real-life crouching: crouch in game when your head is this many metres below where it was at the last
; recentre, stand again 0.1 m above that (so it doesn't flicker)
Global VRRealCrouchEnabled% = True
Global VRCrouchHeight# = 0.45
Global VRRealCrouch% = False        ; head is down right now
Global VRCrouchByHead% = False      ; Crouch was switched on by the head, not the crouch button
Global VRNoCrouchOffset% = False    ; the view follows the real head, so skip the game's crouch camera drop

Global VREyeW%, VREyeH%
Dim VREyeTex%(1)

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
				If VR_PanelOpen() Then
					VR_UpdatePointer()
					XR_SubmitOverlay(BufferD3D9Surface(BackBuffer()), 2)
				Else
					XR_SubmitOverlay(BufferD3D9Surface(BackBuffer()), 1)
				EndIf
				XR_EndFrame()
				VR_DrawMirror()
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
	Local drop# = -XR_Head(1) / VRWorldScale ; metres below the standing height from the last recentre
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

	If handleInput Then VR_UpdateRealCrouch()

	PositionEntity Camera, EntityX(Collider), EntityY(Collider), EntityZ(Collider)
	RotateEntity Camera, 0, VRBodyYaw, 0
	If VRNoCrouchOffset Then
		MoveEntity Camera, 0, 0.6, 0 ; crouched for real: the head pose below already lowers the view
	Else
		MoveEntity Camera, 0, 0.6 + CrouchState * -0.3, 0
	EndIf
	MoveEntity Camera, XR_Head(0), XR_Head(1), XR_Head(2)
	TurnEntity Camera, XR_Head(3), XR_Head(4), XR_Head(5)
	user_camera_pitch = EntityPitch(Camera, True)

	; walking follows where you look
	RotateEntity Collider, EntityPitch(Collider), EntityYaw(Camera, True), EntityRoll(Collider)
	VRLastColliderYaw = EntityYaw(Collider)
End Function

;---------------------------------------------------------------------------------------------------- rendering

; Used by RenderWorld2 in place of RenderWorld for the main 3D pass.
Function RenderWorldMain()
	If Not VR_RenderEyes() Then RenderWorld()
End Function

Function VR_RenderEyes%()
	If Not VR_StereoActive() Then Return False

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

	; the desktop mirror is drawn in VR_Flip, after the 2D layer has gone to the headset
	SetBuffer BackBuffer()
	Cls
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
