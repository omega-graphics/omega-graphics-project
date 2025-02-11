from pyyaml.lib import yaml
import plistlib
import json
import re as Regex
import io
import shutil
import os

TextMateRule = dict[str, str]


class TextMateGrammarProcessor:
    variables: "dict[Regex.Pattern,str]"

    def __init__(self):
        self.variables = {}
        return

    def process_string(self,subject: str) -> str:
        output: str = subject
        for v in self.variables:
            output = v.sub(self.variables[v],output)
        return output

    def process_rule(self, r: TextMateRule):
        subject_1 = r.get("match")
        if subject_1 is not None:
            r["match"] = self.process_string(subject_1)

        subject_2 = r.get("begin")
        if subject_2 is not None:
            r["begin"] = self.process_string(subject_2)

        subject_3 = r.get("end")
        if subject_3 is not None:
            r["end"] = self.process_string(subject_3)

        return

    def build_grammar(self, grammar: str, j: bool, output:str):

        yaml_data = yaml.safe_load(io.open(grammar, "r"))
        variables: "dict[str]" = yaml_data["variables"]
        for v in variables:
            self.variables[Regex.compile(r'{{' + v + '}}', Regex.DOTALL | Regex.MULTILINE)] \
                = self.process_string(variables[v])

        patterns: "list[TextMateRule]" = yaml_data["patterns"]
        # print(patterns)
        for p in patterns:
            self.process_rule(p)
        repo: "dict[str,dict[str,list[TextMateRule]]]" = yaml_data["repository"]
        print(repo)
        for p in repo:
            rule_set = repo[p]
            p = rule_set.get("patterns")
            if p is not None:
                for _p in p:
                    self.process_rule(_p)

        yaml_data.pop("variables")
        if j:
            json.dump(yaml_data, io.open(output, "w"), sort_keys=False, indent=2)
        else:
            plistlib.dump(yaml_data, io.open(output, "wb"), sort_keys=False)
        return


def build_textmate_bundle(name:str,dir:str,grammar: str, plist: str):

    tm_bundle = f"{dir}/{name}.tmbundle"
    tm_lang_file = f"{dir}/{name}.tmLanguage"

    TextMateGrammarProcessor().build_grammar(grammar, False, tm_lang_file)

    os.mkdir(tm_bundle)
    shutil.copy2(plist, os.path.join(tm_bundle, os.path.basename(plist)))
    os.mkdir(f"{tm_bundle}/Syntaxes")
    shutil.copy2(tm_lang_file, os.path.join(f"{tm_bundle}/Syntaxes", tm_lang_file))

    return
