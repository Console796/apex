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
