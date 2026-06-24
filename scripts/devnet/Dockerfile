# Slim runtime image for rippled-pq DevNet nodes.
# Expects a locally pre-built `xrpld` binary (host path resolved by
# docker-compose's build context). xrpld is statically linked against
# everything but libc/libm; trixie ships glibc >= 2.39 which is what xrpld
# built on a recent host expects.
#
# XRPLD_BIN selects which binary to package. It defaults to the hybrid build at
# .build/xrpld; the stock ECC baseline profile overrides it with a binary
# built from a clean develop tree so the two configurations run from one
# Dockerfile.
FROM debian:trixie-slim

ARG XRPLD_BIN=.build/xrpld

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY ${XRPLD_BIN} /usr/local/bin/xrpld

RUN mkdir -p /var/lib/rippled /etc/rippled

EXPOSE 5005 6006 51235

ENTRYPOINT ["/usr/local/bin/xrpld"]
# --start pre-enables every amendment listed in [amendments], but ONLY
# when the database is empty. If a previous run left state in
# /var/lib/rippled (the named volume), the binary logs `[amendments]
# section in config file ignored in favor of data in db/wallet.db` and
# falls back to the normal 2-week voting cycle. Always tear down with
# `docker compose down -v` (or `bash scripts/devnet/clean.sh`) between
# runs so amendments pre-enable at fresh genesis.
CMD ["--conf=/etc/rippled/xrpld.cfg", "--start"]
