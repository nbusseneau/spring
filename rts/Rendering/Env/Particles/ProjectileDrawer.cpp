/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include "ProjectileDrawer.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/GlobalUnsynced.h"
#include "Game/LoadScreen.h"
#include "Lua/LuaParser.h"
#include "Rendering/GroundFlash.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/ShadowHandler.h"
#include "Rendering/Units/UnitDrawer.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/GL/FBO.h"
#include "Rendering/GL/RenderBuffers.h"
#include "Rendering/Shaders/Shader.h"
#include "Rendering/Textures/ColorMap.h"
#include "Rendering/Textures/TextureAtlas.h"
#include "Rendering/Common/ModelDrawerHelpers.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Projectiles/ExplosionGenerator.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Projectiles/PieceProjectile.h"
#include "Rendering/Env/Particles/Classes/FlyingPiece.h"
#include "Sim/Projectiles/WeaponProjectiles/WeaponProjectile.h"
#include "Sim/Weapons/WeaponDefHandler.h"
#include "Sim/Weapons/WeaponDef.h"
#include "System/Config/ConfigHandler.h"
#include "System/EventHandler.h"
#include "System/Exceptions.h"
#include "System/Log/ILog.h"
#include "System/SafeUtil.h"
#include "System/StringUtil.h"
#include "System/ScopedResource.h"
#include <tuple>

CONFIG(int, SoftParticles).defaultValue(1).safemodeValue(0).description("Soften up CEG particles on clipping edges");


static bool CProjectileDrawOrderSortingPredicate(const CProjectile* p1, const CProjectile* p2) noexcept {
	return std::make_tuple(p2->drawOrder, p1->GetSortDist(), p1) > std::make_tuple(p1->drawOrder, p2->GetSortDist(), p2);
}

static bool CProjectileSortingPredicate(const CProjectile* p1, const CProjectile* p2) noexcept {
	return std::make_tuple(p1->GetSortDist(), p1) > std::make_tuple(p2->GetSortDist(), p2);
};


CProjectileDrawer* projectileDrawer = nullptr;

// can not be a CProjectileDrawer; destruction in global
// scope might happen after ~EventHandler (referenced by
// ~EventClient)
alignas(CProjectileDrawer) static std::byte projectileDrawerMem[sizeof(CProjectileDrawer)];


void CProjectileDrawer::InitStatic() {
	if (projectileDrawer == nullptr)
		projectileDrawer = new (projectileDrawerMem) CProjectileDrawer();

	projectileDrawer->Init();
}
void CProjectileDrawer::KillStatic(bool reload) {
	projectileDrawer->Kill();

	if (reload)
		return;

	spring::SafeDestruct(projectileDrawer);
	memset(projectileDrawerMem, 0, sizeof(projectileDrawerMem));
}

void CProjectileDrawer::Init() {
	eventHandler.AddClient(this);

	loadscreen->SetLoadMessage("Creating Projectile Textures");

	textureAtlas = new CTextureAtlas(CTextureAtlas::ATLAS_ALLOC_LEGACY, 0, 0, "ProjectileTextureAtlas", true);
	groundFXAtlas = new CTextureAtlas(CTextureAtlas::ATLAS_ALLOC_LEGACY, 0, 0, "ProjectileEffectsAtlas", true);

	LuaParser resourcesParser("gamedata/resources.lua", SPRING_VFS_MOD_BASE, SPRING_VFS_ZIP);
	LuaParser mapResParser("gamedata/resources_map.lua", SPRING_VFS_MAP_BASE, SPRING_VFS_ZIP);

	resourcesParser.Execute();

	const LuaTable& resTable = resourcesParser.GetRoot();
	const LuaTable& resGraphicsTable = resTable.SubTable("graphics");
	const LuaTable& resProjTexturesTable = resGraphicsTable.SubTable("projectileTextures");
	const LuaTable& resSmokeTexturesTable = resGraphicsTable.SubTable("smoke");
	const LuaTable& resGroundFXTexturesTable = resGraphicsTable.SubTable("groundfx");

	// used to block resources_map.* from overriding any of
	// resources.lua:{projectile, smoke, groundfx}textures,
	// as well as various defaults (repulsegfxtexture, etc)
	spring::unordered_set<std::string> blockedTexNames;

	ParseAtlasTextures(true, resProjTexturesTable, blockedTexNames, textureAtlas);
	ParseAtlasTextures(true, resGroundFXTexturesTable, blockedTexNames, groundFXAtlas);

	int smokeTexCount = -1;

	{
		// get the smoke textures, hold the count in 'smokeTexCount'
		if (resSmokeTexturesTable.IsValid()) {
			for (smokeTexCount = 0; true; smokeTexCount++) {
				const std::string& tex = resSmokeTexturesTable.GetString(smokeTexCount + 1, "");
				if (tex.empty())
					break;

				const std::string texName = "bitmaps/" + tex;
				const std::string smokeName = "ismoke" + IntToString(smokeTexCount, "%02i");

				textureAtlas->AddTexFromFile(smokeName, texName);
				blockedTexNames.insert(StringToLower(smokeName));
			}
		} else {
			// setup the defaults
			for (smokeTexCount = 0; smokeTexCount < 12; smokeTexCount++) {
				const std::string smokeNum = IntToString(smokeTexCount, "%02i");
				const std::string smokeName = "ismoke" + smokeNum;
				const std::string texName = "bitmaps/smoke/smoke" + smokeNum + ".tga";

				textureAtlas->AddTexFromFile(smokeName, texName);
				blockedTexNames.insert(StringToLower(smokeName));
			}
		}

		if (smokeTexCount <= 0) {
			// this needs to be an exception, other code
			// assumes at least one smoke-texture exists
			throw content_error("missing smoke textures");
		}
	}

	{
		// shield-texture memory
		std::array<char, 4 * perlinTexSize * perlinTexSize> perlinTexMem;
		perlinTexMem.fill(70);
		textureAtlas->AddTexFromMem("perlintex", perlinTexSize, perlinTexSize, CTextureAtlas::RGBA32, &perlinTexMem[0]);
		blockedTexNames.insert("perlintex");
	}

	blockedTexNames.insert("flare");
	blockedTexNames.insert("explo");
	blockedTexNames.insert("explofade");
	blockedTexNames.insert("heatcloud");
	blockedTexNames.insert("laserend");
	blockedTexNames.insert("laserfalloff");
	blockedTexNames.insert("randdots");
	blockedTexNames.insert("smoketrail");
	blockedTexNames.insert("wake");
	blockedTexNames.insert("flame");

	blockedTexNames.insert("sbtrailtexture");
	blockedTexNames.insert("missiletrailtexture");
	blockedTexNames.insert("muzzleflametexture");
	blockedTexNames.insert("repulsetexture");
	blockedTexNames.insert("dguntexture");
	blockedTexNames.insert("flareprojectiletexture");
	blockedTexNames.insert("sbflaretexture");
	blockedTexNames.insert("missileflaretexture");
	blockedTexNames.insert("beamlaserflaretexture");
	blockedTexNames.insert("bubbletexture");
	blockedTexNames.insert("geosquaretexture");
	blockedTexNames.insert("gfxtexture");
	blockedTexNames.insert("projectiletexture");
	blockedTexNames.insert("repulsegfxtexture");
	blockedTexNames.insert("sphereparttexture");
	blockedTexNames.insert("torpedotexture");
	blockedTexNames.insert("wrecktexture");
	blockedTexNames.insert("plasmatexture");

	if (mapResParser.Execute()) {
		// allow map-specified atlas textures (for gaia-projectiles and ground-flashes)
		const LuaTable& mapResTable = mapResParser.GetRoot();
		const LuaTable& mapResGraphicsTable = mapResTable.SubTable("graphics");
		const LuaTable& mapResProjTexturesTable = mapResGraphicsTable.SubTable("projectileTextures");
		const LuaTable& mapResGroundFXTexturesTable = mapResGraphicsTable.SubTable("groundfx");

		ParseAtlasTextures(false, mapResProjTexturesTable, blockedTexNames, textureAtlas);
		ParseAtlasTextures(false, mapResGroundFXTexturesTable, blockedTexNames, groundFXAtlas);
	}

	if (!textureAtlas->Finalize())
		LOG_L(L_ERROR, "Could not finalize projectile-texture atlas. Use fewer/smaller textures.");


	flaretex        = &textureAtlas->GetTexture("flare");
	explotex        = &textureAtlas->GetTexture("explo");
	explofadetex    = &textureAtlas->GetTexture("explofade");
	heatcloudtex    = &textureAtlas->GetTexture("heatcloud");
	laserendtex     = &textureAtlas->GetTexture("laserend");
	laserfallofftex = &textureAtlas->GetTexture("laserfalloff");
	randdotstex     = &textureAtlas->GetTexture("randdots");
	smoketrailtex   = &textureAtlas->GetTexture("smoketrail");
	waketex         = &textureAtlas->GetTexture("wake");
	perlintex       = &textureAtlas->GetTexture("perlintex");
	flametex        = &textureAtlas->GetTexture("flame");

	smokeTextures.reserve(smokeTexCount);

	for (int i = 0; i < smokeTexCount; i++) {
		smokeTextures.push_back(&textureAtlas->GetTexture("ismoke" + IntToString(i, "%02i")));
	}

	sbtrailtex         = &textureAtlas->GetTextureWithBackup("sbtrailtexture",         "smoketrail"    );
	missiletrailtex    = &textureAtlas->GetTextureWithBackup("missiletrailtexture",    "smoketrail"    );
	muzzleflametex     = &textureAtlas->GetTextureWithBackup("muzzleflametexture",     "explo"         );
	repulsetex         = &textureAtlas->GetTextureWithBackup("repulsetexture",         "explo"         );
	dguntex            = &textureAtlas->GetTextureWithBackup("dguntexture",            "flare"         );
	flareprojectiletex = &textureAtlas->GetTextureWithBackup("flareprojectiletexture", "flare"         );
	sbflaretex         = &textureAtlas->GetTextureWithBackup("sbflaretexture",         "flare"         );
	missileflaretex    = &textureAtlas->GetTextureWithBackup("missileflaretexture",    "flare"         );
	beamlaserflaretex  = &textureAtlas->GetTextureWithBackup("beamlaserflaretexture",  "flare"         );
	bubbletex          = &textureAtlas->GetTextureWithBackup("bubbletexture",          "circularthingy");
	geosquaretex       = &textureAtlas->GetTextureWithBackup("geosquaretexture",       "circularthingy");
	gfxtex             = &textureAtlas->GetTextureWithBackup("gfxtexture",             "circularthingy");
	projectiletex      = &textureAtlas->GetTextureWithBackup("projectiletexture",      "circularthingy");
	repulsegfxtex      = &textureAtlas->GetTextureWithBackup("repulsegfxtexture",      "circularthingy");
	sphereparttex      = &textureAtlas->GetTextureWithBackup("sphereparttexture",      "circularthingy");
	torpedotex         = &textureAtlas->GetTextureWithBackup("torpedotexture",         "circularthingy");
	wrecktex           = &textureAtlas->GetTextureWithBackup("wrecktexture",           "circularthingy");
	plasmatex          = &textureAtlas->GetTextureWithBackup("plasmatexture",          "circularthingy");


	if (!groundFXAtlas->Finalize())
		LOG_L(L_ERROR, "Could not finalize groundFX texture atlas. Use fewer/smaller textures.");

	groundflashtex = &groundFXAtlas->GetTexture("groundflash");
	groundringtex = &groundFXAtlas->GetTexture("groundring");
	seismictex = &groundFXAtlas->GetTexture("seismic");


	for (int a = 0; a < 4; ++a) {
		perlinBlend[a] = 0.0f;
	}

	{
		glGenTextures(8, perlinBlendTex);
		for (int a = 0; a < 8; ++a) {
			glBindTexture(GL_TEXTURE_2D, perlinBlendTex[a]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, perlinBlendTexSize, perlinBlendTexSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		}
	}


	// ProjectileDrawer is no-op constructed, has to be initialized manually
	perlinFB.Init(false);

	if (perlinFB.IsValid()) {
		// we never refresh the full texture (just the perlin part), so reload it on AT
		perlinFB.reloadOnAltTab = true;

		perlinFB.Bind();
		perlinFB.AttachTexture(textureAtlas->GetTexID());
		drawPerlinTex = perlinFB.CheckStatus("PROJECTILE-DRAWER-PERLIN");
		perlinFB.Unbind();
	}


	modellessProjectiles.reserve(projectileHandler.maxParticles + projectileHandler.maxNanoParticles);
	for (auto& mr : modelRenderers) { mr.Clear(); }

	LoadWeaponTextures();

	{
		fsShadowShader = shaderHandler->CreateProgramObject("[ProjectileDrawer::VFS]", "FX Shader shadow");

		fsShadowShader->AttachShaderObject(shaderHandler->CreateShaderObject("GLSL/ProjFXVertShadowProg.glsl", "", GL_VERTEX_SHADER));
		fsShadowShader->AttachShaderObject(shaderHandler->CreateShaderObject("GLSL/ProjFXFragShadowProg.glsl", "", GL_FRAGMENT_SHADER));

		{
			using VAT = std::decay_t<decltype(CProjectile::GetPrimaryRenderBuffer())>::VertType;
			fsShadowShader->BindAttribLocations<VAT>();
		}

		fsShadowShader->Link();
		fsShadowShader->Enable();

		fsShadowShader->SetUniform("atlasTex", 0);
		fsShadowShader->SetUniform("alphaCtrl", 0.0f, 1.0f, 0.0f, 0.0f);
		fsShadowShader->SetUniform("shadowColorMode", shadowHandler.shadowColorMode > 0 ? 1.0f : 0.0f);

		fsShadowShader->Disable();
		fsShadowShader->Validate();
	}

	fxShaders[0] = shaderHandler->CreateProgramObject("[ProjectileDrawer::VFS]", "FX Shader hard");
	fxShaders[1] = shaderHandler->CreateProgramObject("[ProjectileDrawer::VFS]", "FX Shader soft");

	for (auto*& fxShader : fxShaders)
	{
		fxShader->AttachShaderObject(shaderHandler->CreateShaderObject("GLSL/ProjFXVertProg.glsl", "", GL_VERTEX_SHADER));
		fxShader->AttachShaderObject(shaderHandler->CreateShaderObject("GLSL/ProjFXFragProg.glsl", "", GL_FRAGMENT_SHADER));

		{
			using VAT = std::decay_t<decltype(CProjectile::GetPrimaryRenderBuffer())>::VertType;
			fxShader->BindAttribLocations<VAT>();
		}

		fxShader->SetFlag("DEPTH_CLIP01", globalRendering->supportClipSpaceControl);
		if (fxShader == fxShaders[1])
			fxShader->SetFlag("SMOOTH_PARTICLES", CheckSoftenExt());

		fxShader->Link();
		fxShader->Enable();
		fxShader->SetUniform("atlasTex", 0);
		if (fxShader == fxShaders[1]) {
			fxShader->SetUniform("depthTex", 15);
			fxShader->SetUniform("softenExponent", softenExponent[0], softenExponent[1]);
		}
		fxShader->Disable();

		fxShader->Validate();
	}
	ViewResize();
	EnableSoften(configHandler->GetInt("SoftParticles"));
}

void CProjectileDrawer::Kill() {
	eventHandler.RemoveClient(this);
	autoLinkedEvents.clear();

	glDeleteTextures(8, perlinBlendTex);
	spring::SafeDelete(textureAtlas);
	spring::SafeDelete(groundFXAtlas);

	smokeTextures.clear();

	modellessProjectiles.clear();
	sortedProjectiles.clear();
	unsortedProjectiles.clear();

	perlinFB.Kill();

	perlinTexObjects = 0;
	drawPerlinTex = false;

	drawSorted = true;

	shaderHandler->ReleaseProgramObjects("[ProjectileDrawer::VFS]");
	fxShaders = { nullptr };
	fsShadowShader = nullptr;

	if (depthFBO) {
		if (depthFBO->IsValid()) {
			depthFBO->Bind();
			depthFBO->DetachAll();
			depthFBO->Unbind();
		}
		depthFBO->Kill();
		spring::SafeDelete(depthFBO);
	}

	if (depthTexture > 0u) {
		glDeleteTextures(1, &depthTexture);
		depthTexture = 0u;
	}

	configHandler->Set("SoftParticles", wantSoften);
}

void CProjectileDrawer::ViewResize()
{
	if (!CheckSoftenExt())
		return;

	if (depthTexture != 0u) {
		glDeleteTextures(1, &depthTexture);
		depthTexture = 0u;
	}
	glGenTextures(1, &depthTexture);

	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, depthTexture);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0); //might break something else
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

	GLint depthFormat = static_cast<GLint>(CGlobalRendering::DepthBitsToFormat(globalRendering->supportDepthBufferBitDepth));
	glTexImage2D(GL_TEXTURE_2D, 0, depthFormat, globalRendering->viewSizeX, globalRendering->viewSizeY, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

	glBindTexture(GL_TEXTURE_2D, 0);

	if (depthFBO) {
		if (depthFBO->IsValid()) {
			depthFBO->Bind();
			depthFBO->DetachAll();
			depthFBO->Unbind();
		}
		depthFBO->Kill();
		spring::SafeDelete(depthFBO); //probably redundant
	}

	depthFBO = new FBO(); //probably redundant
	depthFBO->Init(false);

	depthFBO->Bind();
	depthFBO->AttachTexture(depthTexture, GL_TEXTURE_2D, GL_DEPTH_ATTACHMENT_EXT);
	glDrawBuffer(GL_NONE);
	depthFBO->CheckStatus("PROJECTILE-DRAWER-DEPTHFBO");
	depthFBO->Unbind();
}

bool CProjectileDrawer::CheckSoftenExt()
{
	static bool result =
		FBO::IsSupported() &&
		GLEW_EXT_framebuffer_blit &&
		globalRendering->haveGLSL; //eval once
	return result;
}

void CProjectileDrawer::CopyDepthBufferToTexture()
{
	if (lastDrawFrame == globalRendering->drawFrame) //copy once per draw frame
		return;

#if 1
	//no need to touch glViewport
	const std::array<int, 4> srcScreenRect = { globalRendering->viewPosX, globalRendering->viewPosY, globalRendering->viewPosX + globalRendering->viewSizeX, globalRendering->viewPosY + globalRendering->viewSizeY };
	const std::array<int, 4> dstScreenRect = { 0, 0, globalRendering->viewSizeX, globalRendering->viewSizeY };

	FBO::Blit(-1, depthFBO->GetId(), srcScreenRect, dstScreenRect, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
#else
	GLint activeTex;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
	glActiveTexture(GL_TEXTURE15); glBindTexture(GL_TEXTURE_2D, depthTexture);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, globalRendering->viewPosX, 0, globalRendering->viewSizeX, globalRendering->viewSizeY);
	glActiveTexture(activeTex);
#endif

	lastDrawFrame = globalRendering->drawFrame;
}

void CProjectileDrawer::ParseAtlasTextures(
	const bool blockTextures,
	const LuaTable& textureTable,
	spring::unordered_set<std::string>& blockedTextures,
	CTextureAtlas* texAtlas
) {
	std::vector<std::string> subTables;
	spring::unordered_map<std::string, std::string> texturesMap;

	textureTable.GetMap(texturesMap);
	textureTable.GetKeys(subTables);

	for (auto texturesMapIt = texturesMap.begin(); texturesMapIt != texturesMap.end(); ++texturesMapIt) {
		const std::string textureName = StringToLower(texturesMapIt->first);

		// no textures added to this atlas are allowed
		// to be overwritten later by other textures of
		// the same name
		if (blockTextures)
			blockedTextures.insert(textureName);

		if (blockTextures || (blockedTextures.find(textureName) == blockedTextures.end()))
			texAtlas->AddTexFromFile(texturesMapIt->first, "bitmaps/" + texturesMapIt->second);
	}

	texturesMap.clear();

	for (size_t i = 0; i < subTables.size(); i++) {
		const LuaTable& textureSubTable = textureTable.SubTable(subTables[i]);

		if (!textureSubTable.IsValid())
			continue;

		textureSubTable.GetMap(texturesMap);

		for (auto texturesMapIt = texturesMap.begin(); texturesMapIt != texturesMap.end(); ++texturesMapIt) {
			const std::string textureName = StringToLower(texturesMapIt->first);

			if (blockTextures)
				blockedTextures.insert(textureName);

			if (blockTextures || (blockedTextures.find(textureName) == blockedTextures.end()))
				texAtlas->AddTexFromFile(texturesMapIt->first, "bitmaps/" + texturesMapIt->second);
		}

		texturesMap.clear();
	}
}

void CProjectileDrawer::LoadWeaponTextures() {
	// post-process the synced weapon-defs to set unsynced fields
	// (this requires CWeaponDefHandler to have been initialized)
	for (WeaponDef& wd: const_cast<std::vector<WeaponDef>&>(weaponDefHandler->GetWeaponDefsVec())) {
		wd.visuals.texture1 = nullptr;
		wd.visuals.texture2 = nullptr;
		wd.visuals.texture3 = nullptr;
		wd.visuals.texture4 = nullptr;

		if (!wd.visuals.colorMapStr.empty())
			wd.visuals.colorMap = CColorMap::LoadFromDefString(wd.visuals.colorMapStr);

		if (wd.type == "Cannon") {
			wd.visuals.texture1 = plasmatex;
		} else if (wd.type == "AircraftBomb") {
			wd.visuals.texture1 = plasmatex;
		} else if (wd.type == "Shield") {
			wd.visuals.texture1 = perlintex;
		} else if (wd.type == "Flame") {
			wd.visuals.texture1 = flametex;

			if (wd.visuals.colorMap == nullptr) {
				wd.visuals.colorMap = CColorMap::LoadFromDefString(
					"1.0 1.0 1.0 0.1 "
					"0.025 0.025 0.025 0.10 "
					"0.0 0.0 0.0 0.0"
				);
			}
		} else if (wd.type == "MissileLauncher") {
			wd.visuals.texture1 = missileflaretex;
			wd.visuals.texture2 = missiletrailtex;
		} else if (wd.type == "TorpedoLauncher") {
			wd.visuals.texture1 = plasmatex;
		} else if (wd.type == "LaserCannon") {
			wd.visuals.texture1 = laserfallofftex;
			wd.visuals.texture2 = laserendtex;
		} else if (wd.type == "BeamLaser") {
			if (wd.largeBeamLaser) {
				wd.visuals.texture1 = &textureAtlas->GetTexture("largebeam");
				wd.visuals.texture2 = laserendtex;
				wd.visuals.texture3 = &textureAtlas->GetTexture("muzzleside");
				wd.visuals.texture4 = beamlaserflaretex;
			} else {
				wd.visuals.texture1 = laserfallofftex;
				wd.visuals.texture2 = laserendtex;
				wd.visuals.texture3 = beamlaserflaretex;
			}
		} else if (wd.type == "LightningCannon") {
			wd.visuals.texture1 = laserfallofftex;
		} else if (wd.type == "EmgCannon") {
			wd.visuals.texture1 = plasmatex;
		} else if (wd.type == "StarburstLauncher") {
			wd.visuals.texture1 = sbflaretex;
			wd.visuals.texture2 = sbtrailtex;
			wd.visuals.texture3 = explotex;
		} else {
			wd.visuals.texture1 = plasmatex;
			wd.visuals.texture2 = plasmatex;
		}

		// override the textures if we have specified names for them
		if (!wd.visuals.texNames[0].empty()) { wd.visuals.texture1 = &textureAtlas->GetTexture(wd.visuals.texNames[0]); }
		if (!wd.visuals.texNames[1].empty()) { wd.visuals.texture2 = &textureAtlas->GetTexture(wd.visuals.texNames[1]); }
		if (!wd.visuals.texNames[2].empty()) { wd.visuals.texture3 = &textureAtlas->GetTexture(wd.visuals.texNames[2]); }
		if (!wd.visuals.texNames[3].empty()) { wd.visuals.texture4 = &textureAtlas->GetTexture(wd.visuals.texNames[3]); }

		// trails can only be custom EG's, prefix is not required game-side
		if (!wd.visuals.ptrailExpGenTag.empty())
			wd.ptrailExplosionGeneratorID = explGenHandler.LoadCustomGeneratorID(wd.visuals.ptrailExpGenTag.c_str());

		if (!wd.visuals.impactExpGenTag.empty())
			wd.impactExplosionGeneratorID = explGenHandler.LoadGeneratorID(wd.visuals.impactExpGenTag.c_str());

		if (!wd.visuals.bounceExpGenTag.empty())
			wd.bounceExplosionGeneratorID = explGenHandler.LoadGeneratorID(wd.visuals.bounceExpGenTag.c_str());
	}
}



void CProjectileDrawer::DrawProjectiles(int modelType, bool drawReflection, bool drawRefraction)
{
	const auto& mdlRenderer = modelRenderers[modelType];
	// const auto& projBinKeys = mdlRenderer.GetObjectBinKeys();

	for (unsigned int i = 0, n = mdlRenderer.GetNumObjectBins(); i < n; i++) {
		CModelDrawerHelper::BindModelTypeTexture(modelType, mdlRenderer.GetObjectBinKey(i));
		DrawProjectilesSet(mdlRenderer.GetObjectBin(i), drawReflection, drawRefraction);
	}

	DrawFlyingPieces(modelType);
}

void CProjectileDrawer::DrawProjectilesSet(const std::vector<CProjectile*>& projectiles, bool drawReflection, bool drawRefraction)
{
	for (CProjectile* p: projectiles) {
		DrawProjectileNow(p, drawReflection, drawRefraction);
	}
}

bool CProjectileDrawer::CanDrawProjectile(const CProjectile* pro, int allyTeam)
{
	auto& th = teamHandler;
	auto& lh = losHandler;
	return (gu->spectatingFullView || (th.IsValidAllyTeam(allyTeam) && th.Ally(allyTeam, gu->myAllyTeam)) || lh->InLos(pro, gu->myAllyTeam));
}

void CProjectileDrawer::DrawProjectileNow(CProjectile* pro, bool drawReflection, bool drawRefraction)
{
	pro->drawPos = pro->GetDrawPos(globalRendering->timeOffset);

	if (!CanDrawProjectile(pro, pro->GetAllyteamID()))
		return;


	if (drawRefraction && (pro->drawPos.y > pro->GetDrawRadius()) /*!pro->IsInWater()*/)
		return;
	// removed this to fix AMD particle drawing
	//if (drawReflection && !CModelDrawerHelper::ObjectVisibleReflection(pro->drawPos, camera->GetPos(), pro->GetDrawRadius()))
	//	return;

	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (!cam->InView(pro->drawPos, pro->GetDrawRadius()))
		return;

	// no-op if no model
	DrawProjectileModel(pro);

	pro->SetSortDist(cam->ProjectedDistance(pro->pos));

	if (drawSorted && pro->drawSorted) {
		sortedProjectiles.emplace_back(pro);
	} else {
		unsortedProjectiles.emplace_back(pro);
	}

}



void CProjectileDrawer::DrawProjectilesShadow(int modelType)
{
	const auto& mdlRenderer = modelRenderers[modelType];
	// const auto& projBinKeys = mdlRenderer.GetObjectBinKeys();

	for (unsigned int i = 0, n = mdlRenderer.GetNumObjectBins(); i < n; i++) {
		DrawProjectilesSetShadow(mdlRenderer.GetObjectBin(i));
	}

	DrawFlyingPieces(modelType);
}

void CProjectileDrawer::DrawProjectilesSetShadow(const std::vector<CProjectile*>& projectiles)
{
	for (CProjectile* p: projectiles) {
		DrawProjectileShadow(p);
	}
}

void CProjectileDrawer::DrawProjectileShadow(CProjectile* p)
{
	if (CanDrawProjectile(p, p->GetAllyteamID())) {
		const CCamera* cam = CCameraHandler::GetActiveCamera();
		if (!cam->InView(p->drawPos, p->GetDrawRadius()))
			return;

		if (!p->castShadow)
			return;

		// if this returns false, then projectile is
		// neither weapon nor piece, or has no model
		if (DrawProjectileModel(p))
			return;

		// don't need to z-sort in the shadow pass
		p->Draw();
	}
}



void CProjectileDrawer::DrawProjectilesMiniMap()
{
	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_CNT; modelType++) {
		const auto& mdlRenderer = modelRenderers[modelType];
		// const auto& projBinKeys = mdlRenderer.GetObjectBinKeys();

		for (unsigned int i = 0, n = mdlRenderer.GetNumObjectBins(); i < n; i++) {
			const auto& projectileBin = mdlRenderer.GetObjectBin(i);

			for (CProjectile* p: projectileBin) {
				if (!CanDrawProjectile(p, p->GetAllyteamID()))
					continue;

				p->DrawOnMinimap();
			}
		}
	}

	if (!modellessProjectiles.empty()) {
		for (CProjectile* p: modellessProjectiles) {
			if (!CanDrawProjectile(p, p->GetAllyteamID()))
				continue;

			p->DrawOnMinimap();
		}
	}

	auto& sh = TypedRenderBuffer<VA_TYPE_C>::GetShader();

	glLineWidth(1.0f);

	// Note: glPointSize(1.0f); doesn't work here on AMD drivers.
	// AMD drivers draw huge circles instead of small point for some reason
	// so disable GL_PROGRAM_POINT_SIZE
	const bool pntsz = glIsEnabled(GL_PROGRAM_POINT_SIZE);
	if (pntsz)
		glDisable(GL_PROGRAM_POINT_SIZE);

	sh.Enable();
	CProjectile::GetMiniMapLinesRB().DrawArrays(GL_LINES);
	CProjectile::GetMiniMapPointsRB().DrawArrays(GL_POINTS);
	sh.Disable();

	if (pntsz)
		glEnable(GL_PROGRAM_POINT_SIZE);
}

void CProjectileDrawer::DrawFlyingPieces(int modelType) const
{
	const FlyingPieceContainer& container = projectileHandler.flyingPieces[modelType];

	if (container.empty())
		return;

	FlyingPiece::BeginDraw();

	const FlyingPiece* last = nullptr;
	for (const FlyingPiece& fp: container) {
		const bool noLosTst = gu->spectatingFullView || teamHandler.AlliedTeams(gu->myTeam, fp.GetTeam());
		const bool inAirLos = noLosTst || losHandler->InAirLos(fp.GetPos(), gu->myAllyTeam);

		if (!inAirLos)
			continue;

		if (!camera->InView(fp.GetPos(), fp.GetRadius()))
			continue;

		fp.Draw(last);
		last = &fp;
	}

	FlyingPiece::EndDraw();
}


void CProjectileDrawer::Draw(bool drawReflection, bool drawRefraction) {
	glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
	glDisable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glDepthMask(GL_TRUE);

	ISky::GetSky()->SetupFog();


	sortedProjectiles.clear();
	unsortedProjectiles.clear();

	{
		{
			ScopedModelDrawerImpl<CUnitDrawer> legacy(true, false);
			unitDrawer->SetupOpaqueDrawing(false);

			for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_CNT; modelType++) {
				CModelDrawerHelper::PushModelRenderState(modelType);
				DrawProjectiles(modelType, drawReflection, drawRefraction);
				CModelDrawerHelper::PopModelRenderState(modelType);
			}

			unitDrawer->ResetOpaqueDrawing(false);
		}

		// note: model-less projectiles are NOT drawn by this call but
		// only z-sorted (if the projectiles indicate they want to be)
		DrawProjectilesSet(modellessProjectiles, drawReflection, drawRefraction);

		if (wantDrawOrder)
			std::sort(sortedProjectiles.begin(), sortedProjectiles.end(), CProjectileDrawOrderSortingPredicate);
		else
			std::sort(sortedProjectiles.begin(), sortedProjectiles.end(), CProjectileSortingPredicate);

		for (auto p : sortedProjectiles) {
			p->Draw();
		}

		for (auto p : unsortedProjectiles) {
			p->Draw();
		}
	}

	glEnable(GL_BLEND);
	glDisable(GL_FOG);

	auto& rb = CExpGenSpawnable::GetPrimaryRenderBuffer();

	const bool needSoften = (wantSoften > 0) && !drawReflection && !drawRefraction;

	if (rb.ShouldSubmit()) {
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		/*
		glEnable(GL_TEXTURE_2D);

		glColor4f(1.0f, 1.0f, 1.0f, 0.2f);
		glAlphaFunc(GL_GREATER, 0.0f);
		glEnable(GL_ALPHA_TEST);
		*/

		glDepthMask(GL_FALSE);

		// send event after the default state has been set, allows overriding
		// it for specific cases such as proper blending with depth-aware fog
		// (requires mask=true and func=always)
		eventHandler.DrawWorldPreParticles();

		glActiveTexture(GL_TEXTURE0); textureAtlas->BindTexture();

		if (needSoften) {
			CopyDepthBufferToTexture();
			glActiveTexture(GL_TEXTURE15); glBindTexture(GL_TEXTURE_2D, depthTexture);
		}

		fxShaders[needSoften]->Enable();
		fxShaders[needSoften]->SetUniform("alphaCtrl", 0.0f, 1.0f, 0.0f, 0.0f);
		if (needSoften) {
			fxShaders[needSoften]->SetUniform("softenThreshold", CProjectileDrawer::softenThreshold[0]);
		}

		rb.DrawElements(GL_TRIANGLES);

		fxShaders[needSoften]->Disable();

		if (needSoften) {
			glBindTexture(GL_TEXTURE_2D, 0); //15th slot
			glActiveTexture(GL_TEXTURE0);
		}
		glBindTexture(GL_TEXTURE_2D, 0);
	} else {
		eventHandler.DrawWorldPreParticles();
	}

	glPopAttrib();
}

void CProjectileDrawer::DrawShadowPassOpaque()
{
	Shader::IProgramObject* po = shadowHandler.GetShadowGenProg(CShadowHandler::SHADOWGEN_PROGRAM_PROJECTILE);

	glPushAttrib(GL_ENABLE_BIT);
	glDisable(GL_TEXTURE_2D);
	po->Enable();
	{
		for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_CNT; modelType++) {
			DrawProjectilesShadow(modelType);
		}
	}
	po->Disable();

	//glShadeModel(GL_FLAT);
	glPopAttrib();
}

void CProjectileDrawer::DrawShadowPassTransparent()
{
	// Method #1 here: https://wickedengine.net/2018/01/18/easy-transparent-shadow-maps/

	// 1) Render opaque objects into depth stencil texture from light's point of view - done elsewhere

	// draw the model-less projectiles
	DrawProjectilesSetShadow(modellessProjectiles);

	auto& rb = CExpGenSpawnable::GetPrimaryRenderBuffer();
	if (!rb.ShouldSubmit())
		return;

	// 2) Bind render target for shadow color filter: R11G11B10 works good
	shadowHandler.EnableColorOutput(true);

	// 3) Clear render target to 1,1,1,0 (RGBA) color - done elsewhere

	// 4) Apply depth stencil state with depth read, but no write
	//glEnable(GL_DEPTH_TEST); - already enabled
	glDepthMask(GL_FALSE);

	// 5) Apply multiplicative blend state eg:
	// SrcBlend = BLEND_ZERO
	//	DestBlend = BLEND_SRC_COLOR
	//	BlendOp = BLEND_OP_ADD
	glBlendFunc(GL_ZERO, GL_SRC_COLOR);
	glEnable(GL_BLEND);

	// 6) Render transparents in arbitrary order
	textureAtlas->BindTexture();
	fsShadowShader->Enable();
	fsShadowShader->SetUniform("shadowColorMode", shadowHandler.shadowColorMode > 0 ? 1.0f : 0.0f);

	rb.DrawElements(GL_TRIANGLES);

	fsShadowShader->Disable();
	glBindTexture(GL_TEXTURE_2D, 0);

	//shadowHandler.EnableColorOutput(false);

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_BLEND);

	glDepthMask(GL_TRUE);
}



bool CProjectileDrawer::DrawProjectileModel(const CProjectile* p)
{
	if (p->model == nullptr)
		return false;

	ScopedModelDrawerImpl<CUnitDrawer> legacy(true, false);

	switch ((p->weapon * 2) + (p->piece * 1)) {
		case 2: {
			// weapon-projectile
			const CWeaponProjectile* wp = static_cast<const CWeaponProjectile*>(p);

			CUnitDrawer::SetTeamColor(wp->GetTeamID());

			glPushMatrix();
				glMultMatrixf(wp->GetTransformMatrix(wp->GetProjectileType() == WEAPON_MISSILE_PROJECTILE));

				if (!p->luaDraw || !eventHandler.DrawProjectile(p))
					wp->model->DrawStatic();

			glPopMatrix();
			return true;
		} break;

		case 1: {
			// piece-projectile
			const CPieceProjectile* pp = static_cast<const CPieceProjectile*>(p);

			CUnitDrawer::SetTeamColor(pp->GetTeamID());

			auto scopedPushPop = spring::ScopedNullResource(glPushMatrix, glPopMatrix);

			glTranslatef3(pp->drawPos);
			glRotatef(pp->GetDrawAngle(), pp->spinVec.x, pp->spinVec.y, pp->spinVec.z);

			if (p->luaDraw && eventHandler.DrawProjectile(p)) {
				return true;
			}

			if ((pp->explFlags & PF_Recursive) != 0) {
				pp->omp->DrawStaticLegacyRec();
			}
			else {
				// non-recursive, only draw one piece
				pp->omp->DrawStaticLegacy(true, false);
			}

			return true;
		} break;

		default: {
		} break;
	}

	return false;
}

void CProjectileDrawer::DrawGroundFlashes()
{
	const GroundFlashContainer& gfc = projectileHandler.groundFlashes;

	if (gfc.empty())
		return;

	static constexpr GLfloat black[] = {0.0f, 0.0f, 0.0f, 0.0f};

	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glActiveTexture(GL_TEXTURE0);
	groundFXAtlas->BindTexture();
/*
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.01f);
*/
	glPolygonOffset(-20, -1000);
	glEnable(GL_POLYGON_OFFSET_FILL);
//	glFogfv(GL_FOG_COLOR, black);

	bool depthTest = true;
	bool depthMask = false;

	const bool needSoften = (wantSoften > 0);

	auto& rb = CExpGenSpawnable::GetPrimaryRenderBuffer();

	if (needSoften) {
		CopyDepthBufferToTexture();
		glActiveTexture(GL_TEXTURE15); glBindTexture(GL_TEXTURE_2D, depthTexture);
	}

	fxShaders[needSoften]->Enable();
	fxShaders[needSoften]->SetUniform("alphaCtrl", 0.01f, 1.0f, 0.0f, 0.0f);
	if (needSoften) {
		fxShaders[needSoften]->SetUniform("softenThreshold", -CProjectileDrawer::softenThreshold[1]);
	}

	for (CGroundFlash* gf: gfc) {
		const bool inLos = gf->alwaysVisible || gu->spectatingFullView || losHandler->InAirLos(gf, gu->myAllyTeam);
		if (!inLos)
			continue;

		if (!camera->InView(gf->pos, gf->size))
			continue;

		bool depthTestWanted = needSoften ? false : gf->depthTest;

		if (depthTest != depthTestWanted || depthMask != gf->depthMask) {
			rb.DrawElements(GL_TRIANGLES);

			if ((depthTest = depthTestWanted)) {
				glEnable(GL_DEPTH_TEST);
			} else {
				glDisable(GL_DEPTH_TEST);
			}

			if ((depthMask = gf->depthMask)) {
				glDepthMask(GL_TRUE);
			} else {
				glDepthMask(GL_FALSE);
			}
		}

		gf->Draw();
	}

	rb.DrawElements(GL_TRIANGLES);

	fxShaders[needSoften]->Disable();

	if (needSoften) {
		glBindTexture(GL_TEXTURE_2D, 0); //15th slot
		glActiveTexture(GL_TEXTURE0);
	}
	glBindTexture(GL_TEXTURE_2D, 0);

//	glFogfv(GL_FOG_COLOR, sky->fogColor);
	glDisable(GL_POLYGON_OFFSET_FILL);
//	glDisable(GL_ALPHA_TEST);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
}



void CProjectileDrawer::UpdateTextures() {
	if (perlinTexObjects > 0 && drawPerlinTex)
		UpdatePerlin();
}

void CProjectileDrawer::UpdatePerlin() {
	perlinFB.Bind();
	glViewport(perlintex->xstart * (textureAtlas->GetSize()).x, perlintex->ystart * (textureAtlas->GetSize()).y, perlinTexSize, perlinTexSize);

	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glLoadMatrixf(CMatrix44f::ClipOrthoProj01());
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);

	unsigned char col[4];
	float time = globalRendering->lastFrameTime * gs->speedFactor * 0.003f;
	float speed = 1.0f;
	float size = 1.0f;

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_TC>();
	rb.AssertSubmission();

	auto& sh = rb.GetShader();
	sh.Enable();
	sh.SetUniform("alphaCtrl", 0.0f, 0.0f, 0.0f, 1.0f); // no test
	sh.Disable();

	for (int a = 0; a < 4; ++a) {
		perlinBlend[a] += time * speed;
		if (perlinBlend[a] > 1) {
			unsigned int temp = perlinBlendTex[a * 2];
			perlinBlendTex[a * 2    ] = perlinBlendTex[a * 2 + 1];
			perlinBlendTex[a * 2 + 1] = temp;

			GenerateNoiseTex(perlinBlendTex[a * 2 + 1]);
			perlinBlend[a] -= 1;
		}

		float tsize = 8.0f / size;

		if (a == 0)
			glDisable(GL_BLEND);

		for (int b = 0; b < 4; ++b)
			col[b] = int((1.0f - perlinBlend[a]) * 16 * size);

		glBindTexture(GL_TEXTURE_2D, perlinBlendTex[a * 2]);

		rb.AddQuadTriangles(
			{ ZeroVector, 0,         0, col },
			{   UpVector, 0,     tsize, col },
			{   XYVector, tsize, tsize, col },
			{  RgtVector, tsize,     0, col }
		);
		sh.Enable();
		rb.DrawElements(GL_TRIANGLES);
		sh.Disable();

		if (a == 0)
			glEnable(GL_BLEND);

		for (int b = 0; b < 4; ++b)
			col[b] = int(perlinBlend[a] * 16 * size);

		glBindTexture(GL_TEXTURE_2D, perlinBlendTex[a * 2 + 1]);

		rb.AddQuadTriangles(
			{ ZeroVector,     0,     0, col },
			{ UpVector  ,     0, tsize, col },
			{ XYVector  , tsize, tsize, col },
			{ RgtVector , tsize,     0, col }
		);
		sh.Enable();
		rb.DrawElements(GL_TRIANGLES);
		sh.Disable();

		speed *= 0.6f;
		size *= 2;
	}

	perlinFB.Unbind();
	globalRendering->LoadViewport();

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);

	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();

	glMatrixMode(GL_MODELVIEW);
}

void CProjectileDrawer::GenerateNoiseTex(unsigned int tex)
{
	std::array<unsigned char, 4 * perlinBlendTexSize * perlinBlendTexSize> mem;

	for (int a = 0; a < perlinBlendTexSize * perlinBlendTexSize; ++a) {
		const unsigned char rnd = int(std::max(0.0f, guRNG.NextFloat() * 555.0f - 300.0f));

		mem[a * 4 + 0] = rnd;
		mem[a * 4 + 1] = rnd;
		mem[a * 4 + 2] = rnd;
		mem[a * 4 + 3] = rnd;
	}

	glBindTexture(GL_TEXTURE_2D, tex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, perlinBlendTexSize, perlinBlendTexSize, GL_RGBA, GL_UNSIGNED_BYTE, &mem[0]);
}



void CProjectileDrawer::RenderProjectileCreated(const CProjectile* p)
{
	if (p->model != nullptr) {
		modelRenderers[MDL_TYPE(p)].AddObject(p);
		return;
	}

	const_cast<CProjectile*>(p)->SetRenderIndex(modellessProjectiles.size());
	modellessProjectiles.push_back(const_cast<CProjectile*>(p));
}

void CProjectileDrawer::RenderProjectileDestroyed(const CProjectile* p)
{
	if (p->model != nullptr) {
		modelRenderers[MDL_TYPE(p)].DelObject(p);
		return;
	}

	const unsigned int idx = p->GetRenderIndex();

	if (idx >= modellessProjectiles.size()) {
		assert(false);
		return;
	}

	modellessProjectiles[idx] = modellessProjectiles.back();
	modellessProjectiles[idx]->SetRenderIndex(idx);

	modellessProjectiles.pop_back();
}

