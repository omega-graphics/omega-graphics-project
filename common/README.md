# OmegaCommon

A cross platform base api providing ADT implementations, cross platform filesystem operations, and a C runtime for C interop with all of our C++ apis.

This repo also features a cross-platform wrapper generator
See [omega-wrapgen](./wrapgen)

## Build
> This library is not built as a standalone library by default as it is used only in our C++ apis.

#### Requirements:

> Clone the [`omega-graphics/autom`](https://github.com/omega-graphics/autom) repo and follow the instructions listed in the README


1. Clone the repo
```sh
autom-clone https://github.com/omega-graphics/omega-wtk-project ./wtk
```
2. Configure the build
```sh
autom --mode gn --out out
```

3. Build the project
```sh
ninja -C out
```





## License

Apache 2.0

See [LICENSE](LICENSE)
