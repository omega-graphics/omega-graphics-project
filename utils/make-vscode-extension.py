import textmate
import os
import argparse

parser = argparse.ArgumentParser()
parser.add_argument("--name",type=str)
parser.add_argument("--version",type=str)
parser.add_argument("--output",type=str)
parser.parse_args()

if __name__ == "__main__":

    output : str = parser.output
    

    if not os.path.exists(f"{output}/vscode/syntaxes"):
        os.mkdir(f"{output}/vscode/syntaxes")
    textmate.TextMateGrammarProcessor().build_grammar(f"{parser.name}.yaml",True,f"{output}/syntaxes/{parser.name}.tmLanguage.json")

    prev_path = os.curdir
    os.chdir(f"{output}/vscode")
    os.system("npm install")
    os.system("npx vsce package")
    os.chdir(prev_path)
