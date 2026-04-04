import argparse
import io
import json
import plistlib
import re
import shutil
import uuid
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover - fallback for older local layouts
    from pyyaml.lib import yaml


class TextMateGrammarProcessor:
    def __init__(self):
        self.variables = {}

    def process_string(self, subject: str) -> str:
        output = subject
        for pattern, replacement in self.variables.items():
            output = pattern.sub(replacement, output)
        return output

    def process_rule(self, rule):
        for key in ("match", "begin", "end"):
            subject = rule.get(key)
            if subject is not None:
                rule[key] = self.process_string(subject)

        for key in ("include",):
            subject = rule.get(key)
            if subject is not None:
                rule[key] = self.process_string(subject)

        for key in ("patterns",):
            nested = rule.get(key)
            if nested is not None:
                for item in nested:
                    self.process_rule(item)

    def load_grammar(self, grammar_path: Path):
        yaml_data = yaml.safe_load(io.open(grammar_path, "r", encoding="utf-8"))

        variables = yaml_data.get("variables", {})
        for name, value in variables.items():
            self.variables[re.compile(r"{{" + re.escape(name) + r"}}", re.MULTILINE | re.DOTALL)] = self.process_string(value)

        for rule in yaml_data.get("patterns", []):
            self.process_rule(rule)

        for repo_rule in yaml_data.get("repository", {}).values():
            self.process_rule(repo_rule)

        yaml_data.pop("variables", None)
        return yaml_data

    def build_grammar(self, grammar_path: Path, output_path: Path, as_json: bool = False):
        grammar = self.load_grammar(grammar_path)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        if as_json:
            with io.open(output_path, "w", encoding="utf-8") as handle:
                json.dump(grammar, handle, indent=2, sort_keys=False)
                handle.write("\n")
        else:
            with io.open(output_path, "wb") as handle:
                plistlib.dump(grammar, handle, sort_keys=False)
        return grammar


def build_bundle_info(grammar: dict) -> dict:
    grammar_uuid = grammar["uuid"]
    bundle_uuid = str(uuid.uuid5(uuid.UUID(grammar_uuid), "omegasl.tmbundle"))
    grammar_name = grammar["name"]
    return {
        "name": grammar_name,
        "description": f"{grammar_name} syntax highlighting bundle",
        "ordering": [grammar_uuid],
        "uuid": bundle_uuid,
    }


def build_vscode_manifest(grammar: dict) -> dict:
    grammar_name = grammar["name"]
    scope_name = grammar["scopeName"]
    language_id = "omegasl"
    syntax_filename = f"{grammar_name}.tmLanguage.json"
    return {
        "name": "omegasl-vscode",
        "displayName": grammar_name,
        "description": "VS Code support for the OmegaSL shading language.",
        "version": "0.1.0",
        "publisher": "unpublished",
        "license": "UNLICENSED",
        "engines": {
            "vscode": "^1.75.0",
        },
        "categories": [
            "Programming Languages",
        ],
        "contributes": {
            "languages": [
                {
                    "id": language_id,
                    "aliases": [
                        grammar_name,
                    ],
                    "extensions": [
                        ".omegasl",
                    ],
                    "configuration": "./language-configuration.json",
                }
            ],
            "grammars": [
                {
                    "language": language_id,
                    "scopeName": scope_name,
                    "path": f"./syntaxes/{syntax_filename}",
                }
            ],
        },
    }


def build_textmate_bundle(grammar_path: Path, output_dir: Path, bundle_name: str | None = None):
    processor = TextMateGrammarProcessor()
    grammar = processor.load_grammar(grammar_path)

    grammar_name = grammar["name"]
    bundle_root_name = bundle_name or f"{grammar_name}.tmbundle"
    tm_language_name = f"{grammar_name}.tmLanguage"

    tm_language_path = output_dir / tm_language_name
    bundle_path = output_dir / bundle_root_name
    syntaxes_path = bundle_path / "Syntaxes"
    info_plist_path = bundle_path / "Info.plist"

    output_dir.mkdir(parents=True, exist_ok=True)
    if bundle_path.exists():
        shutil.rmtree(bundle_path)

    processor.build_grammar(grammar_path, tm_language_path, as_json=False)

    syntaxes_path.mkdir(parents=True, exist_ok=True)
    shutil.copy2(tm_language_path, syntaxes_path / tm_language_name)

    with io.open(info_plist_path, "wb") as handle:
        plistlib.dump(build_bundle_info(grammar), handle, sort_keys=False)

    return tm_language_path, bundle_path


def build_vscode_extension(grammar_path: Path, vscode_dir: Path):
    processor = TextMateGrammarProcessor()
    grammar = processor.load_grammar(grammar_path)

    syntaxes_dir = vscode_dir / "syntaxes"
    syntaxes_dir.mkdir(parents=True, exist_ok=True)

    json_output = syntaxes_dir / f"{grammar['name']}.tmLanguage.json"
    processor.build_grammar(grammar_path, json_output, as_json=True)

    package_path = vscode_dir / "package.json"
    with io.open(package_path, "w", encoding="utf-8") as handle:
        json.dump(build_vscode_manifest(grammar), handle, indent=2, sort_keys=False)
        handle.write("\n")

    return json_output, package_path


def parse_args():
    script_dir = Path(__file__).resolve().parent
    default_grammar = script_dir / "syntax" / "omegasl.yaml"
    default_output_dir = script_dir / "syntax"

    parser = argparse.ArgumentParser(description="Build OmegaSL TextMate grammar artifacts from YAML.")
    parser.add_argument("--grammar", type=Path, default=default_grammar, help="Path to the OmegaSL YAML grammar.")
    parser.add_argument("--output-dir", type=Path, default=default_output_dir, help="Directory for generated artifacts.")
    parser.add_argument("--bundle-name", default="OmegaSL.tmbundle", help="Name of the generated TextMate bundle directory.")
    parser.add_argument("--json", action="store_true", help="Emit a JSON grammar alongside the plist grammar.")
    parser.add_argument("--vscode-dir", type=Path, help="Optional VS Code extension directory to populate with a syntax JSON and package manifest.")
    return parser.parse_args()


def main():
    args = parse_args()
    grammar_path = args.grammar.resolve()
    output_dir = args.output_dir.resolve()

    tm_language_path, bundle_path = build_textmate_bundle(grammar_path, output_dir, args.bundle_name)

    if args.json:
        json_output = output_dir / "OmegaSL.tmLanguage.json"
        TextMateGrammarProcessor().build_grammar(grammar_path, json_output, as_json=True)
        print(f"Generated {json_output}")

    if args.vscode_dir is not None:
        json_output, package_path = build_vscode_extension(grammar_path, args.vscode_dir.resolve())
        print(f"Generated {json_output}")
        print(f"Generated {package_path}")

    print(f"Generated {tm_language_path}")
    print(f"Generated {bundle_path}")


if __name__ == "__main__":
    main()
