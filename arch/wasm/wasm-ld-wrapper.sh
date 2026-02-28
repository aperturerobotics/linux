#\!/bin/sh
# Wrapper for wasm-ld that handles Linux kernel build system flags.

WASM_LD=$HOME/wasi-sdk/bin/wasm-ld
LLVM_AR=$HOME/wasi-sdk/bin/llvm-ar

# Pass through version/help queries directly
for arg in "$@"; do
    case "$arg" in
        --version|-v|--help|-V)
            exec $WASM_LD "$@"
            ;;
    esac
done

relocatable=0
output=""
script=""
inputs=""

while [ $# -gt 0 ]; do
    case "$1" in
        -r)
            relocatable=1
            ;;
        -o)
            shift
            output="$1"
            ;;
        --script=*)
            script="$1"
            ;;
        -T)
            shift
            script="--script=$1"
            ;;
        --start-group|--end-group|--whole-archive|--no-whole-archive)
            ;;
        --strip-debug)
            ;;
        -Map=*)
            ;;
        -Map)
            shift
            ;;
        --build-id=*)
            ;;
        -z)
            shift
            ;;
        -*)
            # Pass through other flags
            inputs="$inputs $1"
            ;;
        *)
            inputs="$inputs $1"
            ;;
    esac
    shift
done

if [ "$relocatable" = "1" ]; then
    rm -f "$output"
    $LLVM_AR rcsT "$output" $inputs 2>/dev/null || \
    $LLVM_AR rcs "$output" $inputs
    exit $?
fi

ARGS=""
[ -n "$script" ] && ARGS="$ARGS $script"
ARGS="$ARGS --no-entry --import-undefined"
ARGS="$ARGS --export=_initialize --export=tick --export=deliver_io"
ARGS="$ARGS -o $output"
ARGS="$ARGS $inputs"

exec $WASM_LD $ARGS
