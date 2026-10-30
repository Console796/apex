#include "assets/vmt.hpp"
#include "fixture.hpp"

#include <doctest/doctest.h>

using namespace apex;
using namespace apex::assets;

TEST_CASE("VMT extracts editor flags and resolves nested case-insensitive patches") {
    GameTree tree;
    tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    tree.put("materials/base.vmt", R"(LightmappedGeneric {
        $basetexture old $basetexture2 second $surfaceprop metal
        $translucent 1 $alphatest 0 $nocull 1
        %tooltexture editor/tool %ToolViewportTexture editor/viewport
        %compileSkip 1 %compileNonSolid 0
        Proxies { Test { value old } }
    })");
    tree.put("materials/first.vmt", R"(Patch { include "MATERIALS\\BASE.VMT"
        insert { $basetexture ignored $alphatest ignored $new added }
        replace { $basetexture changed $alphatest 1 $absent ignored }
    })");
    tree.put("materials/final.vmt", R"(patch { include first
        replace { $surfaceprop concrete $translucent 0 }
    })");
    JobPool jobs(1);
    GameProfile profile;
    profile.gameDirectory = tree.root;
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    auto material = loadMaterial(*vfs, "FINAL");
    REQUIRE(material);
    CHECK(material->shader == "LightmappedGeneric");
    CHECK(material->baseTexture == "changed");
    CHECK(material->baseTexture2 == "second");
    CHECK(material->params.at("$new") == "added");
    CHECK_FALSE(material->params.contains("$absent"));
    CHECK(material->surfaceProp == "concrete");
    CHECK_FALSE(material->translucent);
    CHECK(material->alphaTest);
    CHECK(material->noCull);
    CHECK(material->toolTexture == "editor/tool");
    CHECK(material->previewTexture() == "editor/viewport");
    CHECK(material->compileFlags.at("%compileskip") == "1");
    CHECK(material->compileFlags.at("%compilenonsolid") == "0");
    CHECK_FALSE(loadMaterial(*vfs, "missing"));
}

TEST_CASE("VMT reports invalid patches cycles and syntax") {
    GameTree tree;
    tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    tree.put("materials/a.vmt", "patch { include b }");
    tree.put("materials/b.vmt", "patch { include A }");
    tree.put("materials/noinclude.vmt", "patch {}");
    tree.put("materials/bad.vmt", "shader {");
    tree.put("materials/value.vmt", "shader value");
    tree.put("materials/directive.vmt", "#include materials/simple.vmt");
    tree.put("materials/simple.vmt", "UnlitGeneric { $basetexture simple }");
    JobPool jobs(1);
    GameProfile profile;
    profile.gameDirectory = tree.root;
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    for (auto name : {"a", "noinclude", "bad", "value"}) {
        CAPTURE(name);
        CHECK_FALSE(loadMaterial(*vfs, name));
    }
    auto material = loadMaterial(*vfs, "directive");
    REQUIRE(material);
    CHECK(material->shader == "UnlitGeneric");
}

TEST_CASE("VMT texture transforms compose scale rotation pivot and translation") {
    const auto read = [](std::string_view) -> Result<std::string> {
        return R"(PBR { $basetexturetransform "translate 0.25 -0.5 rotate 90 scale 2 3 center 0.5 0.5" })";
    };
    const auto material = parseMaterial(read, "test");
    REQUIRE(material);
    const auto pivot = material->baseTextureTransform * DVec3{0.5, 0.5, 1};
    CHECK(pivot.x == doctest::Approx(0.75));
    CHECK(pivot.y == doctest::Approx(0.0));
    const auto point = material->baseTextureTransform * DVec3{1, 1, 1};
    CHECK(point.x == doctest::Approx(-0.75));
    CHECK(point.y == doctest::Approx(1.0));
    for (const auto* value : {"", "scale 2", "scale nope 2", "rotate nan", "unknown 1"}) {
        const auto fallback = parseMaterial([&](std::string_view) -> Result<std::string> {
            return std::string("PBR { $basetexturetransform \"") + value + "\" }";
        }, "test");
        REQUIRE(fallback);
        CHECK(fallback->baseTextureTransform == DMat3{1.0});
    }
}

TEST_CASE("VMT paths keep backslash n and t literal including trailing directory separators") {
    const auto material = parseMaterial([](std::string_view) -> Result<std::string> {
        return R"(PBR {
            $basetexture "models\pbr\test_chambers\testchmb_door_Color"
            $bumpmap "models\normal"
            %tooltexture "tools\nodraw"
            %ToolViewportTexture "textures\tiles\"
        })";
    }, "door");
    REQUIRE(material);
    CHECK(texturePath(material->baseTexture) == "materials/models/pbr/test_chambers/testchmb_door_color.vtf");
    CHECK(texturePath(material->toolTexture) == "materials/tools/nodraw.vtf");
    CHECK(material->toolViewportTexture == R"(textures\tiles\)");
}

TEST_CASE("VMT retains material parameters and selects desktop conditional blocks") {
    const auto material = parseMaterial([](std::string_view) -> Result<std::string> {
        return R"(VertexLitGeneric {
            $baseTexture old $BumpMap old_normal $ssbump 0
            $envmap env_cubemap $envmaptint "[0.2 0.4 0.6]" $envmapmask masks/reflection
            $normalmapalphaenvmapmask 1 $selfillum 1 $selfillumtint "{255 128 0}"
            $phong 1 $phongexponent 32 $phongboost 2.5 $phongfresnelranges "[0.1 0.3 0.9]"
            $phongexponenttexture masks/exponent $unimplemented retained
            >=dx90 { $basetexture desktop $bumpmap desktop_normal $ssbump 1 }
            hdr { $envmaptint "[0.3 0.5 0.7]" }
            <dx90 { $basetexture legacy $bumpmap legacy_normal }
            Proxies { Test { $bumpmap proxy $arbitrary hidden } }
        })";
    }, "test");
    REQUIRE(material);
    CHECK(material->baseTexture == "desktop");
    CHECK(material->bumpMap() == "desktop_normal");
    CHECK(material->ssbump());
    CHECK(material->envmap() == "env_cubemap");
    CHECK(material->envmapTint() == DVec3(0.3, 0.5, 0.7));
    CHECK(material->envmapMask() == "masks/reflection");
    CHECK(material->normalMapAlphaEnvmapMask());
    CHECK(material->selfIllum());
    CHECK(material->selfIllumTint() == DVec3(1, 128.0 / 255, 0));
    CHECK(material->phong());
    CHECK(material->phongExponent() == 32);
    CHECK(material->phongBoost() == 2.5);
    CHECK(material->phongFresnelRanges() == DVec3(0.1, 0.3, 0.9));
    CHECK(material->phongExponentTexture() == "masks/exponent");
    CHECK(material->params.at("$unimplemented") == "retained");
    CHECK_FALSE(material->params.contains("$BumpMap"));
    CHECK_FALSE(material->params.contains("$arbitrary"));
    const Material defaults;
    CHECK(defaults.bumpMap().empty());
    CHECK_FALSE(defaults.ssbump());
    CHECK(defaults.envmap().empty());
    CHECK(defaults.envmapTint() == DVec3(1));
    CHECK(defaults.envmapMask().empty());
    CHECK_FALSE(defaults.normalMapAlphaEnvmapMask());
    CHECK_FALSE(defaults.selfIllum());
    CHECK(defaults.selfIllumTint() == DVec3(1));
    CHECK_FALSE(defaults.phong());
    CHECK(defaults.phongExponent() == 5);
    CHECK(defaults.phongBoost() == 1);
    CHECK(defaults.phongFresnelRanges() == DVec3(0, 0.5, 1));
    CHECK(defaults.phongExponentTexture().empty());
}

TEST_CASE("VMT PBR resolves EverythingPBR shaped three-level patch parameters") {
    const std::map<std::string, std::string> files{
        {"materials/dev/l2_default_model_pbr.vmt", R"(PBR {
            $model 1 $mraoscale "[0.9 1 1]" $envmaptint 0.5 $envmaplightscale 0.5
            $parallaxscale 2 $blendtintbymraoalpha 1 $envmap env_cubemap
        })"},
        {"materials/dev/l2_default_pbr.vmt", R"(patch {
            include "materials\dev\l2_default_model_pbr.vmt" replace { $model 0 }
        })"},
        {"materials/pbr/metal/black_tile_32.vmt", R"(patch {
            include dev/l2_default_pbr
            insert { $basetexture pbr/black_tile_color $bumpmap pbr/black_tile_normal
                     $mraotexture pbr/black_tile_mrao }
        })"}
    };
    const auto material = parseMaterial([&](std::string_view path) -> Result<std::string> {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return std::unexpected(Error{ErrorCode::NotFound, std::string(path)});
        return it->second;
    }, "pbr/metal/black_tile_32");
    REQUIRE(material);
    CHECK(material->shader == "PBR");
    CHECK(material->baseTexture == "pbr/black_tile_color");
    CHECK(material->bumpMap() == "pbr/black_tile_normal");
    CHECK(material->mraoTexture() == "pbr/black_tile_mrao");
    CHECK(material->mraoScale() == DVec3(0.9, 1, 1));
    CHECK_FALSE(material->model());
    CHECK(material->envmapTint() == DVec3(0.5));
    CHECK(material->scalar("$envmaplightscale") == 0.5);
    CHECK(material->envmapLightScale() == 0.5);
    CHECK(material->envmapProbe());
    CHECK(material->scalar("$parallaxscale") == 2);
    CHECK(material->scalar("$blendtintbymraoalpha") == 1);
    CHECK(material->envmap() == "env_cubemap");
    const Material defaults;
    CHECK(defaults.mraoTexture().empty());
    CHECK(defaults.mraoScale() == DVec3(1));
    CHECK_FALSE(defaults.model());
    CHECK(defaults.phongTint() == DVec3(1));
    CHECK_FALSE(defaults.baseMapAlphaPhongMask());
}

TEST_CASE("VMT phong tint mask and PBR model scalars are typed") {
    const auto material = parseMaterial([](std::string_view) -> Result<std::string> {
        return R"(VertexLitGeneric { $phong 1 $phongexponent 24 $phongboost 3
            $phongfresnelranges "[0.2 0.6 1]" $phongtint "[1 0.5 0.25]"
            $phongexponenttexture exponent $basemapalphaphongmask 1 $model 1 })";
    }, "phong");
    REQUIRE(material);
    CHECK(material->phong());
    CHECK(material->phongExponent() == 24);
    CHECK(material->phongBoost() == 3);
    CHECK(material->phongFresnelRanges() == DVec3(0.2, 0.6, 1));
    CHECK(material->phongTint() == DVec3(1, 0.5, 0.25));
    CHECK(material->phongExponentTexture() == "exponent");
    CHECK(material->baseMapAlphaPhongMask());
    CHECK(material->model());
}

TEST_CASE("VMT envmap probe mask selectors and self-illumination mask are typed") {
    const auto material = parseMaterial([](std::string_view) -> Result<std::string> {
        return R"(LightmappedGeneric {
            $basetexture base $envmap ENV_CubeMap $basealphaenvmapmask 1 $envmaplightscale 0.25
            $selfillum 1 $selfillummask masks/glow
        })";
    }, "test");
    REQUIRE(material);
    CHECK(material->envmapProbe()); // Case-insensitive.
    CHECK(material->baseAlphaEnvmapMask());
    CHECK_FALSE(material->normalMapAlphaEnvmapMask());
    CHECK(material->envmapLightScale() == 0.25);
    CHECK(material->selfIllumMask() == "masks/glow");
    Material named;
    named.params["$envmap"] = "env_cubemap_hdr";
    CHECK_FALSE(named.envmapProbe());
    named.params["$envmap"] = "maps/test/cubemap";
    CHECK_FALSE(named.envmapProbe());
    const Material defaults;
    CHECK_FALSE(defaults.envmapProbe());
    CHECK_FALSE(defaults.baseAlphaEnvmapMask());
    CHECK(defaults.envmapLightScale() == 0);
    CHECK(defaults.selfIllumMask().empty());
}
