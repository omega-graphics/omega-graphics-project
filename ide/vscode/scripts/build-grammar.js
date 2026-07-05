'use strict';

// Regenerates ide/vscode/syntaxes/OmegaSL.tmLanguage.json from the canonical
// grammar at gte/omegasl/syntax/omegasl.yaml by delegating to
// ide/utils/build_vscode_ext.py. Run as an npm pre-hook so `compile` and
// `vscode:prepublish` always package a fresh grammar.

const { spawnSync } = require('child_process');
const path = require('path');

const buildScript = path.join(__dirname, '..', '..', 'utils', 'build_vscode_ext.py');
const candidates = process.platform === 'win32' ? ['python', 'python3', 'py'] : ['python3', 'python'];

let result;
for (const cmd of candidates) {
    const args = cmd === 'py' ? ['-3', buildScript, '--no-package'] : [buildScript, '--no-package'];
    result = spawnSync(cmd, args, { stdio: 'inherit' });
    if (!result.error) {
        break;
    }
}

if (!result || result.error) {
    console.error(
        `Could not find a Python interpreter (tried: ${candidates.join(', ')}) to build the OmegaSL grammar.`
    );
    process.exit(1);
}

process.exit(result.status === null ? 1 : result.status);
