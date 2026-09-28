// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <CLI/CLI.hpp>

#include <iostream>
#include <optional>
#include <string>

#include "cli/Commands.h"
#include "cli/GrpcServe.h"
#include "cli/SocketServe.h"

int main(int argc, char **argv)
{
    CLI::App app{ "Genesis host-side tools: inspect, validate, and flatten "
                  "project documents; list packs and commands; manage "
                  "installed extensions; serve the host JSON-RPC API on stdio "
                  "or a local socket." };
    app.set_version_flag("--version", "0.1.0");

    auto *inspect = app.add_subcommand("inspect", "Summarize a project document.");
    std::string inspect_path;
    bool inspect_json = false;
    inspect->add_option("project", inspect_path, "Path to project.json")->required();
    inspect->add_flag("--json", inspect_json, "Emit one JSON object instead of text.");

    auto *validate = app.add_subcommand("validate", "Check a project document.");
    std::string validate_path;
    validate->add_option("project", validate_path, "Path to project.json")->required();

    auto *flatten = app.add_subcommand("flatten", "List a timeline's clips.");
    std::string flatten_path;
    std::optional<std::string> flatten_timeline;
    flatten->add_option("project", flatten_path, "Path to project.json")->required();
    flatten->add_option("--timeline", flatten_timeline,
                        "Timeline id; the active one when omitted.");

    auto *render = app.add_subcommand("render", "Export a project to a video file headlessly.");
    std::string render_path;
    std::string render_out;
    std::optional<std::string> render_preset;
    render->add_option("project", render_path, "Path to project.json")->required();
    render->add_option("--out", render_out, "Output file to write")->required();
    render->add_option("--preset", render_preset,
                       "Export preset name; the first default when omitted.");

    auto *packs = app.add_subcommand("packs", "Inspect and trial effect Packs.");
    auto *packs_list = packs->add_subcommand("list", "List a Pack manifest.");
    std::string packs_path;
    packs_list->add_option("manifest", packs_path, "Path to a pack.toml manifest")->required();
    auto *packs_trial =
            packs->add_subcommand("trial",
                                  "Vet a candidate Pack directory safely: validate the manifest "
                                  "and compile the shader headlessly (never executed, never "
                                  "installed).");
    std::string packs_trial_dir;
    packs_trial->add_option("dir", packs_trial_dir, "Pack directory holding effect.toml")
            ->required();
    bool packs_trial_validate_only = false;
    packs_trial->add_flag("--validate-only", packs_trial_validate_only,
                          "Validate the manifest only; skip the shader compile.");
    packs->require_subcommand(1);

    auto *commands = app.add_subcommand("commands", "List command verbs.");

    auto *extensions = app.add_subcommand(
            "extensions", "Manage the extension store: Packs and native Extensions.");
    auto *extensions_list = extensions->add_subcommand("list", "List installed Extensions.");
    std::string extensions_list_store = genesis::cli::default_extensions_store();
    extensions_list->add_option("--store", extensions_list_store, "Installed-Extension root")
            ->capture_default_str();

    auto *extensions_install =
            extensions->add_subcommand("install",
                                       "Install an Extension from a directory holding "
                                       "extension.toml (native) or effect.toml (Pack), from a git "
                                       "repository (--git), or from a marketplace catalog "
                                       "(--catalog).");
    std::string extensions_install_source;
    extensions_install
            ->add_option("source", extensions_install_source,
                         "Extension directory; with --catalog, the entry id")
            ->expected(0, 1);
    std::string extensions_install_git;
    extensions_install
            ->add_option("--git", extensions_install_git,
                         "Clone and install from this repository URL")
            ->expected(0, 1);
    std::string extensions_install_ref;
    extensions_install
            ->add_option("--ref", extensions_install_ref,
                         "Git branch/tag/commit; the default branch when omitted")
            ->expected(0, 1);
    std::string extensions_install_subdir;
    extensions_install
            ->add_option("--subdir", extensions_install_subdir,
                         "Path inside the repository holding the manifest; the "
                         "repository root when omitted")
            ->expected(0, 1);
    std::string extensions_install_catalog;
    extensions_install
            ->add_option("--catalog", extensions_install_catalog,
                         "Install the entry named by `source` from this catalog URL")
            ->expected(0, 1);
    std::string extensions_install_store = genesis::cli::default_extensions_store();
    extensions_install->add_option("--store", extensions_install_store, "Installed-Extension root")
            ->capture_default_str();
    std::string extensions_install_origin = "user";
    extensions_install
            ->add_option("--origin", extensions_install_origin,
                         "Origin: builtin, curated, developer or user; untrusted "
                         "origins are refused by policy")
            ->capture_default_str();

    auto *extensions_catalog =
            extensions->add_subcommand("catalog", "Fetch and list a marketplace catalog.");
    std::string extensions_catalog_url;
    extensions_catalog->add_option("url", extensions_catalog_url, "Catalog document URL")
            ->required();

    auto *extensions_trust =
            extensions->add_subcommand("trust", "Show the trust policy for an origin.");
    std::string extensions_trust_origin;
    extensions_trust
            ->add_option("origin", extensions_trust_origin,
                         "Origin: builtin, curated, developer or user; all when omitted")
            ->default_val("");

    auto *extensions_remove = extensions->add_subcommand(
            "remove", "Tombstone and delete an installed builtin Extension.");
    std::string extensions_remove_id;
    extensions_remove->add_option("id", extensions_remove_id, "Extension id to remove")->required();
    std::string extensions_remove_store = genesis::cli::default_extensions_store();
    extensions_remove->add_option("--store", extensions_remove_store, "Installed-Extension root")
            ->capture_default_str();

    auto *extensions_restore = extensions->add_subcommand(
            "restore", "Clear an id's tombstone and re-seed the builtins.");
    std::string extensions_restore_id;
    extensions_restore->add_option("id", extensions_restore_id, "Extension id to restore")
            ->required();
    std::string extensions_restore_store = genesis::cli::default_extensions_store();
    extensions_restore->add_option("--store", extensions_restore_store, "Installed-Extension root")
            ->capture_default_str();

    auto *extensions_enable =
            extensions->add_subcommand("enable",
                                       "Enable an installed native Extension by origin (Developer "
                                       "is consent-gated; Builtin/Curated use the disabled list).");
    std::string extensions_enable_id;
    extensions_enable->add_option("id", extensions_enable_id, "Extension id to enable")->required();
    std::string extensions_enable_store = genesis::cli::default_extensions_store();
    extensions_enable->add_option("--store", extensions_enable_store, "Installed-Extension root")
            ->capture_default_str();

    auto *extensions_disable = extensions->add_subcommand(
            "disable", "Disable an installed native Extension and its dependents.");
    std::string extensions_disable_id;
    extensions_disable->add_option("id", extensions_disable_id, "Extension id to disable")
            ->required();
    std::string extensions_disable_store = genesis::cli::default_extensions_store();
    extensions_disable->add_option("--store", extensions_disable_store, "Installed-Extension root")
            ->capture_default_str();

    extensions->require_subcommand(1);

    auto *config = app.add_subcommand("config",
                                      "Export/import a portable profile (settings plus extensions "
                                      "with install-source provenance) as JSON.");
    auto *config_export = config->add_subcommand("export", "Write the store's profile to a file.");
    std::string config_export_file;
    config_export->add_option("file", config_export_file, "Profile file to write")->required();
    std::string config_export_store = genesis::cli::default_extensions_store();
    config_export->add_option("--store", config_export_store, "Installed-Extension root")
            ->capture_default_str();

    auto *config_import = config->add_subcommand("import", "Apply a profile from a file.");
    std::string config_import_file;
    config_import->add_option("file", config_import_file, "Profile file to read")->required();
    std::string config_import_store = genesis::cli::default_extensions_store();
    config_import->add_option("--store", config_import_store, "Installed-Extension root")
            ->capture_default_str();
    bool config_import_no_install = false;
    config_import->add_flag("--no-install", config_import_no_install,
                            "Skip installing extensions; apply only the settings");

    config->require_subcommand(1);

    auto *api = app.add_subcommand("api",
                                   "Serve the host JSON-RPC API on stdio: one JSON request per "
                                   "line on stdin, one JSON reply per line on stdout. Methods are "
                                   "the api/ module's, including edit.apply.");

    auto *serve = app.add_subcommand("serve",
                                     "Serve the host JSON-RPC API over a Unix domain socket: "
                                     "line-delimited JSON-RPC, one client at a time. Exits on "
                                     "SIGINT, SIGTERM, or a `shutdown` request.");
    std::string serve_socket = genesis::cli::default_socket_path();
    serve->add_option("--socket", serve_socket, "Unix domain socket path")->capture_default_str();
#ifdef GENESIS_GRPC_ENABLED
    bool serve_grpc = false;
    std::string serve_grpc_addr = "127.0.0.1:50051";
    std::string serve_token;
    std::string serve_token_file;
    std::optional<std::string> serve_grpc_cert;
    std::optional<std::string> serve_grpc_key;
    serve->add_flag("--grpc", serve_grpc, "Serve over gRPC instead of a Unix socket");
    serve->add_option("--grpc-addr", serve_grpc_addr, "gRPC listen address (localhost by default)")
            ->capture_default_str();
    serve->add_option("--token", serve_token,
                      "Bearer token clients must present for gRPC calls "
                      "(visible in ps; prefer --token-file)")
            ->capture_default_str();
    serve->add_option("--token-file", serve_token_file,
                      "Read the Bearer token from a file (preferred over "
                      "--token)");
    serve->add_option("--grpc-cert", serve_grpc_cert,
                      "TLS server certificate chain (PEM); with --grpc-key "
                      "enables TLS for non-loopback binds");
    serve->add_option("--grpc-key", serve_grpc_key,
                      "TLS server private key (PEM); with --grpc-cert enables "
                      "TLS for non-loopback binds");
#endif

    app.require_subcommand(1);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        return app.exit(e);
    }

    if (inspect->parsed()) {
        return genesis::cli::cmd_inspect(std::cout, inspect_path, inspect_json);
    }
    if (validate->parsed()) {
        return genesis::cli::cmd_validate(std::cout, validate_path);
    }
    if (flatten->parsed()) {
        return genesis::cli::cmd_flatten(std::cout, flatten_path, flatten_timeline);
    }
    if (render->parsed()) {
        return genesis::cli::cmd_render(std::cout, render_path, render_out, render_preset);
    }
    if (packs->parsed()) {
        if (packs_list->parsed()) {
            return genesis::cli::cmd_packs(std::cout, packs_path);
        }
        if (packs_trial->parsed()) {
            return genesis::cli::cmd_packs_trial(std::cout, packs_trial_dir,
                                                 packs_trial_validate_only);
        }
        return genesis::cli::EXIT_FAIL;
    }
    if (commands->parsed()) {
        return genesis::cli::cmd_commands(std::cout);
    }
    if (extensions->parsed()) {
        if (extensions_list->parsed()) {
            return genesis::cli::cmd_extensions_list(std::cout, extensions_list_store);
        }
        if (extensions_install->parsed()) {
            if (!extensions_install_git.empty()) {
                const std::optional<std::string> ref = extensions_install_ref.empty()
                        ? std::nullopt
                        : std::optional<std::string>(extensions_install_ref);
                const std::optional<std::string> subdir = extensions_install_subdir.empty()
                        ? std::nullopt
                        : std::optional<std::string>(extensions_install_subdir);
                return genesis::cli::cmd_extensions_install_git(
                        std::cout, extensions_install_git, ref, subdir, extensions_install_store,
                        extensions_install_origin);
            }
            if (!extensions_install_catalog.empty()) {
                return genesis::cli::cmd_extensions_install_catalog(
                        std::cout, extensions_install_catalog, extensions_install_source,
                        extensions_install_store, extensions_install_origin);
            }
            if (extensions_install_source.empty()) {
                std::cerr << "error: install needs a source directory, --git "
                             "<url>, or --catalog <url>\n";
                return genesis::cli::EXIT_FAIL;
            }
            return genesis::cli::cmd_extensions_install(std::cout, extensions_install_source,
                                                        extensions_install_store,
                                                        extensions_install_origin);
        }
        if (extensions_catalog->parsed()) {
            return genesis::cli::cmd_extensions_catalog(std::cout, extensions_catalog_url);
        }
        if (extensions_trust->parsed()) {
            const std::optional<std::string> origin = extensions_trust_origin.empty()
                    ? std::nullopt
                    : std::optional<std::string>(extensions_trust_origin);
            return genesis::cli::cmd_extensions_trust(std::cout, origin);
        }
        if (extensions_remove->parsed()) {
            return genesis::cli::cmd_extensions_remove(std::cout, extensions_remove_store,
                                                       extensions_remove_id);
        }
        if (extensions_restore->parsed()) {
            return genesis::cli::cmd_extensions_restore(std::cout, extensions_restore_store,
                                                        extensions_restore_id);
        }
        if (extensions_enable->parsed()) {
            return genesis::cli::cmd_extensions_enable(std::cout, extensions_enable_store,
                                                       extensions_enable_id);
        }
        if (extensions_disable->parsed()) {
            return genesis::cli::cmd_extensions_disable(std::cout, extensions_disable_store,
                                                        extensions_disable_id);
        }
        return genesis::cli::EXIT_FAIL;
    }
    if (config->parsed()) {
        if (config_export->parsed()) {
            return genesis::cli::cmd_config_export(std::cout, config_export_file,
                                                   config_export_store);
        }
        if (config_import->parsed()) {
            return genesis::cli::cmd_config_import(std::cout, config_import_file,
                                                   config_import_store, config_import_no_install);
        }
        return genesis::cli::EXIT_FAIL;
    }
    if (api->parsed()) {
        return genesis::cli::cmd_api(std::cin, std::cout);
    }
    if (serve->parsed()) {
#ifdef GENESIS_GRPC_ENABLED
        if (serve_grpc) {
            const genesis::cli::TokenResolution token =
                    genesis::cli::resolve_grpc_token(serve_token, serve_token_file);
            if (!token.error.empty()) {
                std::cerr << "error: " << token.error << '\n';
                return genesis::cli::EXIT_FAIL;
            }
            if (!token.warning.empty()) {
                std::cerr << token.warning << '\n';
            }
            if (!token.token) {
                std::cerr << "error: --grpc requires a token (--token, "
                             "--token-file, or GENESIS_GRPC_TOKEN)\n";
                return genesis::cli::EXIT_FAIL;
            }
            return genesis::cli::cmd_grpc_serve(serve_grpc_addr, *token.token, serve_grpc_cert,
                                                serve_grpc_key);
        }
#endif
        return genesis::cli::cmd_serve(serve_socket);
    }
    return genesis::cli::cmd_commands(std::cout);
}
