// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The single-instance guard's suite: the QLocalServer/QLocalSocket handoff
// between two in-process guard instances, plus the stability of the per-user
// socket name. A first acquire is Primary; a second forwards its path to the
// first (which emits pathReceived) and is Forwarded; a guard whose primary has
// gone away binds cleanly as a fresh Primary. No GUI; Qt Core + Network only.

#include <QCoreApplication>
#include <QString>
#include <QThread>

#include "../support/Checks.h"
#include "../support/Wait.h"

#include "app/support/SingleInstanceGuard.h"

#include <cstdio>
#include <string>

namespace {

using genesis::test::check;
using genesis::test::summary;
using genesis::test::wait_for;

// A process-unique socket name: every guard in this process shares it (so the
// forward handoff finds the primary), but it never collides with the
// production per-user socket default_name() binds.
QString test_socket_name()
{
    return QStringLiteral("genesis-app-test-")
            + QString::number(QCoreApplication::applicationPid());
}

void test_default_name_is_stable()
{
    check(SingleInstanceGuard::default_name() == SingleInstanceGuard::default_name(),
          "the default socket name is stable across calls");
    check(!SingleInstanceGuard::default_name().isEmpty(), "the default socket name is non-empty");
}

void test_primary_then_forward()
{
    const QString name = test_socket_name();
    SingleInstanceGuard first;
    first.set_name(name);
    check(first.acquire(QStringLiteral("/unused/first")) == SingleInstanceGuard::Status::Primary,
          "the first acquire is Primary");

    QString received;
    QObject::connect(&first, &SingleInstanceGuard::pathReceived,
                     [&received](const QString &path) { received = path; });

    SingleInstanceGuard second;
    second.set_name(name);
    const QString forwarded = QStringLiteral("/some/project/folder");
    check(second.acquire(forwarded) == SingleInstanceGuard::Status::Forwarded,
          "the second acquire forwards to the primary");

    check(wait_for([&received] { return !received.isEmpty(); }),
          "the primary receives the forwarded path");
    check(received == forwarded, "the forwarded path arrives verbatim");
}

void test_fresh_primary_after_the_first_is_gone()
{
    const QString name = test_socket_name();
    {
        SingleInstanceGuard first;
        first.set_name(name);
        check(first.acquire(QStringLiteral("/unused/first"))
                      == SingleInstanceGuard::Status::Primary,
              "the first acquire is Primary");
    } // first destroyed: its server and socket are gone

    SingleInstanceGuard second;
    second.set_name(name);
    check(second.acquire(QStringLiteral("/unused/second")) == SingleInstanceGuard::Status::Primary,
          "a fresh guard binds as Primary once the first is gone");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_default_name_is_stable();
    test_primary_then_forward();
    test_fresh_primary_after_the_first_is_gone();

    return summary();
}
