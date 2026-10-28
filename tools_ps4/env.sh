# Sourced by tools_ps4/*.sh: the DolphinPS4 checkout and the console's address.
#
# DolphinPS4 (https://github.com/iHaiDeeZ/DolphinPS4) provides the toolchain
# setup, the Mesa RADV-on-GNM tree and the package script. The build is tested
# against the commit below; DOLPHINPS4=<dir> points at another checkout.
#
# Machine-local settings go in tools_ps4/local.env (gitignored), for example:
#   PS4_HOST=192.168.1.50
#   PS4_FTP_PORT=2121
DOLPHINPS4="${DOLPHINPS4:-$HOME/DolphinPS4}"
DOLPHINPS4_PIN=6297f9d5889f87fe657a3dea784ff8c6b40ed63b

_ge_env_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
[ -f "$_ge_env_dir/local.env" ] && source "$_ge_env_dir/local.env"

# Check the DolphinPS4 checkout and load its toolchain environment (ps4-env.sh).
ge_dolphinps4() {
    [ -f "$DOLPHINPS4/scripts/ps4-env.sh" ] || {
        echo "no DolphinPS4 checkout at $DOLPHINPS4 (clone https://github.com/iHaiDeeZ/DolphinPS4 and" \
             "check out $DOLPHINPS4_PIN, or set DOLPHINPS4)" >&2
        exit 1
    }
    local head
    head="$(git -C "$DOLPHINPS4" rev-parse HEAD 2>/dev/null || echo unknown)"
    [ "$head" = "$DOLPHINPS4_PIN" ] \
        || echo "warning: DolphinPS4 is at $head, tested with $DOLPHINPS4_PIN" >&2
    # shellcheck disable=SC1091
    source "$DOLPHINPS4/scripts/ps4-env.sh"
}

# The console's FTP server (GoldHEN's runs on port 2121).
ge_ps4_ftp() {
    [ -n "${PS4_HOST:-}" ] || { echo "set PS4_HOST (or put it in tools_ps4/local.env)" >&2; exit 1; }
    echo "ftp://$PS4_HOST:${PS4_FTP_PORT:-2121}"
}
