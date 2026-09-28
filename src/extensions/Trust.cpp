// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Trust.h"

namespace genesis::extensions {

bool is_trusted(Origin origin)
{
    switch (origin) {
    case Origin::Builtin:
    case Origin::Curated:
        return true;
    case Origin::Developer:
    case Origin::User:
        return false;
    }
    return false;
}

bool may_install_native(Origin origin)
{
    switch (origin) {
    case Origin::Builtin:
    case Origin::Curated:
    case Origin::Developer:
        return true;
    case Origin::User:
        return false;
    }
    return false;
}

bool may_load_native(Origin origin, bool consented)
{
    switch (origin) {
    case Origin::Builtin:
    case Origin::Curated:
        return true;
    case Origin::Developer:
        return consented;
    case Origin::User:
        return false;
    }
    return false;
}

Permission permissions_for(Origin origin, const genesis::effects::Pack &pack)
{
    // The current Pack schema (format 2) declares no per-Pack capability
    // request: a Pack is a manifest plus a shader, and that shader "cannot do
    // arbitrary IO" (architecture.md §9). The manifest therefore does not
    // further restrict the grant today - `pack` is kept in the signature for
    // future Pack capability fields. The one capability a Pack exercises is
    // contributing its shader, which is granted to the trusted origins and
    // withheld from Developer and User.
    (void)pack;
    Permission permission;
    permission.execute = is_trusted(origin);
    return permission;
}

} // namespace genesis::extensions
