import io 
import json
import os,sys
import re as Regex
import runpy
from enum import Enum
from typing import Any
from urllib.request import *
import tarfile,zipfile,shutil
import argparse,platform
from queue import Queue

# import certifi
import requests


tar_file_regex = Regex.compile(r"(?:\.tar\.(\w{2}))|\.t(\w{2})$",Regex.DOTALL | Regex.MULTILINE)


class Counter:
    _len:int
    current_idx:int = 0
    def __init__(self,_len:int):
        self._len = _len
        s = f"{_len}"
        sys.stdout.write(f"[0/{_len}]\r")
        sys.stdout.flush() # return to start of line, after '['
        sys.stdout.write(f"\n")
    def increment(self):
        self.current_idx+=1
        sys.stdout.write(f"[{self.current_idx}/{self._len}]\r")
        sys.stdout.flush()
        sys.stdout.write(f"\n")
    def finish(self):
       sys.stdout.write(f"\n")


def countAutomDepsFileCommandsRecurse(stream:io.TextIOWrapper) -> int:
    j:dict = json.load(stream)
    current_len = 0

    if j.get("commands") is not None:
        current_len = len(j.get("commands"))

    if j.get("postCommands") is not None:
        current_len += len(j.get('postCommands'))

    if j.get("subdirs") != None:
        subdirs = j.get("subdirs")
        for s in subdirs:
            current_len += countAutomDepsFileCommandsRecurse(io.open(s + "/AUTOMDEPS","r"))
    return current_len

#Global Vars
current_idx:int = 0

_counter:Counter
# Stores the abspaths of all the cloned repos using the clone.py tool waiting to synced with automdeps
clone_automdeps_queue:"list[str]" = []

updateOnly:bool = False
absroot:str
isAbsRoot:bool

Command = dict

variables:"dict[str,Any]" = {}

default_platform:str

# Conditional Parser

def processStringWithVariables(string:str) -> str:
    s = string
    for k in variables:
        # print(k)
        regexExp = Regex.compile(rf"\$\({k}\)",Regex.DOTALL | Regex.MULTILINE)
        if isinstance(variables.get(k),str):
            s = regexExp.sub(variables.get(k),s)
        else:
            s = regexExp.sub(json.dumps(variables.get(k)),s)
    # print(s)
    return s
    
def processCommand(c:Command):
    global default_platform
    if c.get("platforms"):
        platforms: "list[str]" = c.get("platforms")

        sp = default_platform

        cont = False

        for p in platforms:
            if sp == p:
                cont = True
                break
        
        if not cont:
            return

    assert(c.get("type"))
    if c.get("type") == "git_clone":
        assert(c.get("url"))
        assert(c.get("dest"))
        assert(c.get("branch"))
        url = c.get("url")
        url = processStringWithVariables(url)
        dest = c.get("dest")
        dest = processStringWithVariables(dest)
        branch = None
        if(c.get("branch") != "default"):
            branch = c.get("branch")
            branch = processStringWithVariables(branch)

        prior_dir = os.getcwd()
        if updateOnly:
            print(f"Git Pull {c.get('url')}\nBranch:{c.get('branch')}")
            os.chdir(dest)
            os.system(f"git pull")
            os.chdir(prior_dir)
        else:
            print(f"Git Clone {c.get('url')}\nBranch:{c.get('branch')}")
            if branch == None:
                os.system(f"git clone " + url + f" {dest}")
            else:
                os.system(f"git clone " + url + f" --branch {branch} {dest}")

        
    elif c.get("type") == "clone":
        # Same implementation as clone.py
        assert(c.get("url"))
        assert(c.get("dest"))
        assert(c.get("branch"))
        url = processStringWithVariables(c.get("url"))
        dest = processStringWithVariables(c.get("dest"))
        branch = None
        if(c.get("branch") != "default"):
            branch = c.get("branch")
            branch = processStringWithVariables(branch)

        prior_dir = os.getcwd()
        if updateOnly:
            print(f"AUTOM Sync {c.get('url')}\nBranch:{c.get('branch')}")
            os.chdir(dest)
            os.system(f"git pull")
            os.chdir(prior_dir)	
        else:
            print(f"AUTOM Clone {c.get('url')}\nBranch:{c.get('branch')}")
            if branch == None:
                os.system(f"git clone " + url + f" {dest}")
            else:
                os.system(f"git clone " + url + f" --branch {branch} {dest}")
        
        clone_automdeps_queue.append(os.path.abspath(dest))
    else:
        if updateOnly == True:
            print("SKIP")  
    if updateOnly == False:
        if c.get('type') == "chdir":
            assert(c.get("dir"))
            dir = processStringWithVariables(c.get('dir'))
            os.chdir(dir)
        elif c.get('type') == "system":
           assert(c.get("path"))
           path = processStringWithVariables(c.get("path"))
           os.system(path)
        elif c.get("type") == "script":
            assert(c.get("path"))
            assert(c.get("args"))
            path = c.get('path')
            path = processStringWithVariables(path)
            args = c.get("args")
            for arg in args:
                arg = processStringWithVariables(arg)
            print(f"Script {path}")
            sys.argv[1:] = args
            prev = os.path.abspath(os.getcwd())
            os.chdir(os.path.dirname(path))
            root_m,ext_m = os.path.splitext(path)
            print(root_m)
            runpy.run_module(root_m,run_name="__main__",alter_sys=True)
            os.chdir(prev)
        elif c.get("type") == "download":
            assert(c.get("url"))
            assert(c.get("dest"))
            url = c.get('url')
            url = processStringWithVariables(url)
            dest = c.get("dest")
            dest = processStringWithVariables(dest)
            if not os.path.exists(os.path.dirname(dest)):
                os.makedirs(os.path.dirname(dest))
            print(f"Download {url}")
            res = requests.get(url)
            w = open(dest,mode="wb")
            w.write(res.content)
            w.close()
            print(res.ok)
        elif c.get("type") == "tar":
            assert(c.get("tarfile"))
            assert(c.get("dest"))
            dest = c.get("dest")
            dest = processStringWithVariables(dest)
            t_file:str = c.get("tarfile")
            t_file = processStringWithVariables(t_file)
            print(f"Tar {t_file}")
            tar = tarfile.open(t_file,"r:*")
            tar.extractall(dest)
            tar.close()
            os.remove(t_file)
            shutil.rmtree(os.path.dirname(t_file))
        elif c.get("type") == "unzip":
            assert(c.get("zipfile"))
            assert(c.get("dest"))
            dest = c.get("dest")
            dest = processStringWithVariables(dest)
            z_file:str  = c.get("zipfile")
            z_file = processStringWithVariables(z_file)
            _zip = zipfile.ZipFile(z_file,"r")
            print(f"Unzipping {z_file}")
            _zip.extractall(dest)
            _zip.close()
            os.remove(z_file)
            shutil.rmtree(os.path.dirname(z_file))
    return


postCommands: "Queue[list[Command]]" = Queue()
postRootCommands: "list[list[Command]]" = []
priorPostCommandsLen: int


def parseAutomDepsFile(stream:io.TextIOWrapper,root:bool = True,count = 0):
    global updateOnly
    global _counter
    global isAbsRoot
    global variables
    
    j:dict = json.load(stream)

    if isAbsRoot and j.get("rootCommands"):
        count += len(j.get("rootCommands"))
    if root:
        _counter = Counter(count)

    commands: "list[dict]" = []

    if j.get("commands") is not None:
        commands = j.get("commands")

    _local_vars = j.get("variables")
    if _local_vars is not None:
        variables.update(_local_vars)

    if isAbsRoot:
        if j.get("rootCommands") is not None:
            rootCommands = j.get("rootCommands")
            for c in rootCommands:
                processCommand(c)
                _counter.increment()

        isAbsRoot = False

    global postCommands
    global postRootCommands
    global priorPostCommandsLen

    if j.get('postCommands') is not None:
        priorPostCommandsLen = len(postCommands)
        postCommands.put(j.get('postCommands'))
    
    if j.get('postRootCommands') is not None:
        postRootCommands.put(j.get('postRootCommands'))

    for c in commands:
        processCommand(c)
        _counter.increment()

    if j.get("subdirs") is not None:
        subdirs = j.get("subdirs")
        for s in subdirs:
            parent_dir = os.path.abspath(os.getcwd())
            t = os.path.abspath(s)
            os.chdir(t)
            print(f"Invoking sub-directory {os.path.relpath(t,start=absroot)}")
            parseAutomDepsFile(io.open(t + "/AUTOMDEPS","r"),root=False)
            os.chdir(parent_dir)
            if not postCommands.empty():
                if len(postCommands) > priorPostCommandsLen:
                    p_cmd_list = postCommands.get()
                    for c in p_cmd_list:
                        processCommand(c)
                        _counter.increment()
        
    stream.close()


    if root:
        _counter.finish()


    if isAbsRoot and len(postRootCommands) > 0:
        print("Post Commands:")
        counter_len = 0 
        for cmd_list in postRootCommands:
            counter_len += len(cmd_list)
        _counter = Counter(counter_len)
        for cmd_list in postRootCommands:
            for c in cmd_list:
                processCommand(c)
                _counter.increment()
        _counter.finish()


def main(args):
    parser = argparse.ArgumentParser(prog="automdeps",description=
    "AUTOM Project Dependency Manager (Automates 3rd party library installation/fetching as well project configuration)")
    parser.add_argument("--exec", action="store_const", const=True, default=True)
    parser.add_argument("--sync", dest="update", action="store_const", const=True, default=False)
    parser.add_argument("--target", nargs="?", choices=["windows","macos","linux","ios","android"], dest="target")
    args = parser.parse_args(args)

    global default_platform

    if not args.target:
        t_sys = platform.system()
        if t_sys == "Windows":
            default_platform = "windows"
        elif t_sys == "Darwin":
            default_platform = "macos"
        else:
            default_platform = "linux"
    else:
        default_platform = args.target

    global updateOnly
    updateOnly = args.update
       
    global absroot
    absroot = os.path.abspath(os.getcwd())
    if os.path.exists("./AUTOMDEPS"):
        global isAbsRoot
        isAbsRoot = True

        print("Invoking root ./AUTOMDEPS")
        c = countAutomDepsFileCommandsRecurse(io.open("./AUTOMDEPS","r"))
        parseAutomDepsFile(io.open("./AUTOMDEPS","r"),True,c)

        for t in clone_automdeps_queue:
            print(f"Invoking sub-component {os.path.relpath(t,start=absroot)}")
            os.chdir(t)
            c = countAutomDepsFileCommandsRecurse(io.open("./AUTOMDEPS","r"))
            parseAutomDepsFile(io.open("./AUTOMDEPS","r"),True,c)
            os.chdir(absroot)
            
    else:
        raise "AUTOMDEPS File Not Found in Current Directory. Exiting..."
    return


if __name__ == "__main__":
    sys.argv.pop(0)
    main(sys.argv)
    
