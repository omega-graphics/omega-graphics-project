# AUTOM 
## (Automate,Automatic,etc..)

An open-source native code build system generator used by the OmegaGraphicsProject. It can generate to a few build systems including Xcode, and Ninja.



## Setup

Prerequisites:

- Python (3.4 or greater)

- (For Windows Users Only) Visual Studio 

#### Unix

```sh
git clone https://github.com/omega-graphics/autom ./autom
cd ./autom
python3 ./init.py

# Add ./bin to PATH
export PATH=$PATH:~/autom/bin
```

#### Windows

```bat
git clone https://github.com/omega-graphics/autom ./autom
cd autom
py -3 init.py

rem Add ./bin to PATH

set PATH="%PATH%;C:\Users\example-user\autom\bin"
```



## License 

BSD 3-Clause

See [LICENSE](LICENSE)