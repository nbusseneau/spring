/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "System/mmgr.h"

#include "FPSController.h"

#include "Game/Camera.h"
#include "Game/GlobalUnsynced.h"
#include "Map/Ground.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#include "System/myMath.h"

using std::min;
using std::max;

CONFIG(int, FPSScrollSpeed).defaultValue(10);
CONFIG(float, FPSMouseScale).defaultValue(0.01f);
CONFIG(bool, FPSEnabled).defaultValue(true);
CONFIG(float, FPSFOV).defaultValue(45.0f);


CFPSController::CFPSController()
	: oldHeight(300)
{
	scrollSpeed = configHandler->GetInt("FPSScrollSpeed") * 0.1f;
	mouseScale = configHandler->GetFloat("FPSMouseScale");
	enabled = configHandler->GetBool("FPSEnabled");
	fov = configHandler->GetFloat("FPSFOV");
}


void CFPSController::KeyMove(float3 move)
{
	move *= move.z * 400;
	pos  += (camera->forward * move.y + camera->right * move.x) * scrollSpeed;
}


void CFPSController::MouseMove(float3 move)
{
	camera->rot.y -= mouseScale * move.x;
	camera->rot.x -= mouseScale * move.y * move.z;
	camera->rot.x = Clamp(camera->rot.x, -PI*0.4999f, PI*0.4999f);
}


void CFPSController::ScreenEdgeMove(float3 move)
{
	KeyMove(move);
}


void CFPSController::MouseWheelMove(float move)
{
	pos += camera->up * move;
}


float3 CFPSController::GetPos()
{
	if (!gu->fpsMode) {
		const float margin = 0.01f;
		const float xMin = margin;
		const float zMin = margin;
		const float xMax = (float)(gs->mapx * SQUARE_SIZE) - margin;
		const float zMax = (float)(gs->mapy * SQUARE_SIZE) - margin;

		pos.x = Clamp(pos.x, xMin, xMax);
		pos.z = Clamp(pos.z, zMin, zMax);

		const float gndHeight = ground->GetHeightAboveWater(pos.x, pos.z, false);
		const float yMin = gndHeight + 5.0f;
		const float yMax = 9000.0f;
		pos.y = Clamp(pos.y, yMin, yMax);
		oldHeight = pos.y - gndHeight;
	}

	return pos;
}


float3 CFPSController::GetDir()
{
	dir.x = (float)(cos(camera->rot.x) * sin(camera->rot.y));
	dir.z = (float)(cos(camera->rot.x) * cos(camera->rot.y));
	dir.y = (float)(sin(camera->rot.x));
	dir.ANormalize();
	return dir;
}


void CFPSController::SetPos(const float3& newPos)
{
	CCameraController::SetPos(newPos);

	if (!gu->fpsMode) {
		pos.y = ground->GetHeightAboveWater(pos.x, pos.z, false) + oldHeight;
	}
}


void CFPSController::SetDir(const float3& newDir)
{
	dir = newDir;
}


float3 CFPSController::SwitchFrom() const
{
	return pos;
}


void CFPSController::SwitchTo(bool showText)
{
	if (showText) {
		LOG("Switching to FPS style camera");
	}
}


void CFPSController::GetState(StateMap& sm) const
{
	sm["px"] = pos.x;
	sm["py"] = pos.y;
	sm["pz"] = pos.z;

	sm["dx"] = dir.x;
	sm["dy"] = dir.y;
	sm["dz"] = dir.z;

	sm["rx"] = camera->rot.x;
	sm["ry"] = camera->rot.y;
	sm["rz"] = camera->rot.z;

	sm["oldHeight"] = oldHeight;
}


bool CFPSController::SetState(const StateMap& sm)
{
	SetStateFloat(sm, "px", pos.x);
	SetStateFloat(sm, "py", pos.y);
	SetStateFloat(sm, "pz", pos.z);

	SetStateFloat(sm, "dx", dir.x);
	SetStateFloat(sm, "dy", dir.y);
	SetStateFloat(sm, "dz", dir.z);

	SetStateFloat(sm, "rx", camera->rot.x);
	SetStateFloat(sm, "ry", camera->rot.y);
	SetStateFloat(sm, "rz", camera->rot.z);

	SetStateFloat(sm, "oldHeight", oldHeight);

	return true;
}


