// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "effects/PackSource.h"

#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "effects/PackLoader.h"
#include "effects/Resolve.h"

namespace genesis::effects {

BodyResult read_body(const std::filesystem::path &pack_dir)
{
    BodyResult result;
    const std::filesystem::path body_path = pack_dir / GLSL_BODY;
    std::ifstream in(body_path, std::ios::binary);
    if (!in) {
        result.problem = "no GLSL body: " + body_path.string()
                + " is not there (an effect.wgsl beside it is ignored)";
        return result;
    }
    result.body =
            std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return result;
}

SourceResult resolve_source(const std::filesystem::path &pack_dir,
                            const genesis::project::AppliedFilter &instance, double at)
{
    SourceResult result;

    const LoadResult loaded = load_pack_file(pack_dir / "effect.toml");
    if (!loaded.pack) {
        result.problem = "manifest does not load:";
        for (const LoadError &error : loaded.problems) {
            result.problem += " " + error.where + " - " + error.what + ";";
        }
        return result;
    }
    if (!is_expressible(*loaded.pack)) {
        result.problem = "not expressible: the pack draws named intermediates";
        return result;
    }

    std::optional<genesis::core::ShaderPass> pass = resolve_pass(*loaded.pack, instance, at);
    if (!pass) {
        result.problem = "the instance does not resolve";
        return result;
    }

    const BodyResult body = read_body(pack_dir);
    if (!body.body) {
        result.problem = body.problem;
        return result;
    }
    pass->source = std::make_shared<const std::string>(std::move(*body.body));
    result.pass = std::move(pass);
    return result;
}

} // namespace genesis::effects
