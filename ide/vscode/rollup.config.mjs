import { builtinModules } from "node:module";

import commonjs from "@rollup/plugin-commonjs";
import json from "@rollup/plugin-json";
import resolve from "@rollup/plugin-node-resolve";
import typescript from "@rollup/plugin-typescript";

// "vscode" is injected by the extension host at runtime, and Node builtins
// are always available there too -- neither should be inlined into the bundle.
const external = new Set([
    "vscode",
    ...builtinModules,
    ...builtinModules.map((mod) => `node:${mod}`),
]);

export default {
    input: "src/extension.ts",
    output: {
        file: "out/extension.js",
        format: "cjs",
        sourcemap: true,
        exports: "named",
    },
    external: (id) => external.has(id),
    plugins: [
        resolve({ preferBuiltins: true, exportConditions: ["node"] }),
        commonjs(),
        json(),
        typescript({ tsconfig: "./tsconfig.json" }),
    ],
};
