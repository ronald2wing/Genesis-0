// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The bounded GET helper's suite: proves `http_get` returns a local `file://`
// body, a nullopt for a missing file, and a nullopt when a connection stalls
// past the transfer timeout. Headless - it never leaves the loopback interface
// and reads/writes only temp files, so it needs Qt Core + Network and nothing
// else.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <QCoreApplication>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include "app/net/HttpGet.h"

#include "../support/Checks.h"

namespace {

using genesis::test::check;
using genesis::test::summary;

std::string write_temp(const std::string &name, const std::string &contents)
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "genesis-httpget-test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name;
    std::ofstream out(file, std::ios::binary);
    out << contents;
    return file.string();
}

void test_file_fetch_returns_the_body()
{
    const std::string path = write_temp("body.txt", "hello http_get\n");
    const std::optional<std::string> body = genesis::app::http_get("file://" + path);
    check(body.has_value(), "a file:// fetch returns a body");
    if (body.has_value()) {
        check(*body == "hello http_get\n", "the body is the file's bytes");
    }
}

void test_missing_file_is_nullopt()
{
    const std::filesystem::path missing =
            std::filesystem::temp_directory_path() / "genesis-httpget-test" / "does-not-exist.txt";
    std::filesystem::remove(missing);
    const std::optional<std::string> body = genesis::app::http_get("file://" + missing.string());
    check(!body.has_value(), "a missing file:// fetch is nullopt");
}

void test_stalled_connection_times_out()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        check(false, "a local stalled server can listen");
        return;
    }
    const quint16 port = server.serverPort();
    // Accept every connection but never write a response, so the client's
    // transfer stalls and the timeout must lapse rather than the reply erroring
    // cleanly.
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
        while (server.hasPendingConnections()) {
            QTcpSocket *socket = server.nextPendingConnection();
            socket->setParent(&server);
        }
    });

    const std::optional<std::string> body =
            genesis::app::http_get("http://127.0.0.1:" + std::to_string(port) + "/", 500);
    check(!body.has_value(), "a stalled connection lapses the timeout to nullopt");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_file_fetch_returns_the_body();
    test_missing_file_is_nullopt();
    test_stalled_connection_times_out();

    return summary();
}
