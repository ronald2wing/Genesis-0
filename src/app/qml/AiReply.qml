// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The shared decoder for a host reply envelope, so every AI panel/dialog reads
// the same JSON-RPC shape the CLI sees without carrying its own copy:
//
//   {"result": ...}            -> the result
//   {"error": {code, message}} -> null, and the message lands on errorTarget
//   malformed JSON             -> null, and a fixed message lands on errorTarget
//
// A panel instantiates one and points `errorTarget` at itself (its root id),
// then calls `reply.resultOf(...)`. It is a plain QtObject, not a singleton, so
// each panel owns its own error target and two open panels never race.

import QtQuick

QtObject {
    // The object whose `errorText` a malformed or refused reply writes to.
    // Each panel/dialog sets this to its own root.
    property var errorTarget: null

    function resultOf(reply) {
        let parsed;
        try {
            parsed = JSON.parse(reply);
        } catch (e) {
            if (errorTarget)
                errorTarget.errorText = "Malformed host reply";
            return null;
        }
        if (parsed.error !== undefined) {
            if (errorTarget)
                errorTarget.errorText = parsed.error.message !== undefined ? parsed.error.message :
                                                                             "Host refused the request";
            return null;
        }
        if (parsed.result === undefined) {
            if (errorTarget)
                errorTarget.errorText = "Host reply carried no result";
            return null;
        }
        return parsed.result;
    }
}
