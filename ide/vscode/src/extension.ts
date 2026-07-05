import * as fs from "fs";
import * as vscode from "vscode";
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;
let output: vscode.LogOutputChannel | undefined;

const CONFIG_SECTION = "omegasl";
const LANGUAGE_ID = "omegasl";

function log(message: string) {
    if (!output) {
        output = vscode.window.createOutputChannel("OmegaSL", { log: true });
    }
    output.appendLine(message);
}

function resolveServerPath(raw: string): string | undefined {
    if (!raw) {
        return undefined;
    }
    const trimmed = raw.trim();
    if (trimmed.length === 0) {
        return undefined;
    }
    if (!fs.existsSync(trimmed)) {
        log(`omegasl.lspServerPath points to "${trimmed}" but no such file exists; staying in syntax-only mode.`);
        return undefined;
    }
    return trimmed;
}

async function startClient(serverPath: string, serverArgs: string[]): Promise<void> {
    const serverOptions: ServerOptions = {
        run: { command: serverPath, args: serverArgs, transport: TransportKind.stdio },
        debug: { command: serverPath, args: serverArgs, transport: TransportKind.stdio },
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: "file", language: LANGUAGE_ID }],
        outputChannel: output,
    };

    client = new LanguageClient("omegasl", "OmegaSL Language Server", serverOptions, clientOptions);
    log(`Starting omegasl-lsp: ${serverPath} ${serverArgs.join(" ")}`);
    await client.start();
}

async function stopClient(): Promise<void> {
    if (!client) {
        return;
    }
    try {
        await client.stop();
    } catch (err) {
        log(`Error stopping omegasl-lsp: ${err}`);
    }
    client = undefined;
}

async function applyConfig(): Promise<void> {
    const config = vscode.workspace.getConfiguration(CONFIG_SECTION);
    const serverPath = resolveServerPath(config.get<string>("lspServerPath", ""));
    const serverArgs = config.get<string[]>("lspServerArgs", []);

    await stopClient();

    if (!serverPath) {
        log("OmegaSL LSP server not configured -- syntax-only mode.");
        return;
    }

    try {
        await startClient(serverPath, serverArgs);
    } catch (err) {
        log(`Failed to start omegasl-lsp: ${err}`);
    }
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
    output = vscode.window.createOutputChannel("OmegaSL", { log: true });
    context.subscriptions.push(output);

    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration(async (event) => {
            if (event.affectsConfiguration(CONFIG_SECTION)) {
                await applyConfig();
            }
        }),
    );

    context.subscriptions.push({
        dispose: () => {
            void stopClient();
        },
    });

    await applyConfig();
}

export async function deactivate(): Promise<void> {
    await stopClient();
}
