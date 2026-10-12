#!/usr/bin/env python3
#
# FILE            ftJwt.py
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# Keys, JWK Sets and access tokens for the auth functests - the tokens VCVerifier issues (doc/authorization.md
# § 5.1): RS256 or ES256, a kid, iss / aud / iat / exp, and the credential in verifiableCredential. The
# standard library and the openssl command only.
#
#   ftJwt.py key  --out DIR --name N [--alg RS256|ES256]       DIR/N.pem, and its JWK as DIR/N.jwk (kid = N)
#   ftJwt.py jwks --out FILE JWK...                            a JWK Set of the keys given (DIR/N.jwk files)
#   ftJwt.py sign --key DIR/N.pem [--alg RS256|ES256] [--kid K] [--iss I] [--aud A] [--exp SECONDS]
#                 [--vc JSON] [--vp JSON] [--claims JSON] [--badSig]
#                                                              the token, on stdout. --exp: from now
#                                                              (negative: expired); --vp: a JSON array -
#                                                              verifiablePresentation; --claims: merged in
#                                                              last; --badSig: the signature broken
#
import argparse
import base64
import json
import os
import subprocess
import sys
import time


def b64u(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def openssl(*args, data=None):
    return subprocess.run(["openssl", *args], input=data, capture_output=True, check=True).stdout


def derLength(buf, i):
    n = buf[i]
    if n < 0x80:
        return n, i + 1
    k = n & 0x7F
    return int.from_bytes(buf[i + 1:i + 1 + k], "big"), i + 1 + k


def ecdsaRaw(der):
    # SEQUENCE { INTEGER r, INTEGER s } -> r|s, 32 bytes each (RFC 7518 § 3.4)
    _, i = derLength(der, 1)
    out = b""
    for _ in range(2):
        assert der[i] == 0x02
        n, i = derLength(der, i + 1)
        v = der[i:i + n].lstrip(b"\x00")
        out += v.rjust(32, b"\x00")
        i += n
    return out


def cmdKey(a):
    pem = os.path.join(a.out, a.name + ".pem")
    if a.alg == "RS256":
        openssl("genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", pem)
        modulus = openssl("rsa", "-in", pem, "-noout", "-modulus").decode().strip().split("=", 1)[1]
        jwk = {"kty": "RSA", "kid": a.name, "n": b64u(bytes.fromhex(modulus)), "e": b64u((65537).to_bytes(3, "big"))}
    else:
        openssl("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", pem)
        der = openssl("pkey", "-in", pem, "-pubout", "-outform", "DER")
        point = der[-65:]                            # the uncompressed point, 04|x|y, ends the SubjectPublicKeyInfo
        jwk = {"kty": "EC", "kid": a.name, "crv": "P-256", "x": b64u(point[1:33]), "y": b64u(point[33:])}
    with open(os.path.join(a.out, a.name + ".jwk"), "w") as f:
        json.dump(jwk, f)


def cmdJwks(a):
    keys = []
    for path in a.jwk:
        with open(path) as f:
            keys.append(json.load(f))
    with open(a.out, "w") as f:
        json.dump({"keys": keys}, f)


def cmdSign(a):
    kid = a.kid if a.kid is not None else os.path.splitext(os.path.basename(a.key))[0]
    header = {"alg": a.alg, "kid": kid, "typ": "JWT"}
    now = int(time.time())
    claims = {"iss": a.iss, "aud": [a.aud], "iat": now, "exp": now + a.exp, "nonce": "ft"}
    if a.vc is not None:
        claims["verifiableCredential"] = json.loads(a.vc)
    if a.vp is not None:
        claims["verifiablePresentation"] = json.loads(a.vp)
    if a.claims is not None:
        claims.update(json.loads(a.claims))
    signingInput = b64u(json.dumps(header, separators=(",", ":")).encode()) + "." + b64u(json.dumps(claims, separators=(",", ":")).encode())
    sig = openssl("dgst", "-sha256", "-sign", a.key, data=signingInput.encode())
    if a.alg == "ES256":
        sig = ecdsaRaw(sig)
    if a.badSig:
        sig = bytes([sig[0] ^ 0xFF]) + sig[1:]
    print(signingInput + "." + b64u(sig))


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)

    k = sub.add_parser("key")
    k.add_argument("--out", required=True)
    k.add_argument("--name", required=True)
    k.add_argument("--alg", default="RS256", choices=["RS256", "ES256"])

    j = sub.add_parser("jwks")
    j.add_argument("--out", required=True)
    j.add_argument("jwk", nargs="+")

    s = sub.add_parser("sign")
    s.add_argument("--key", required=True)
    s.add_argument("--alg", default="RS256", choices=["RS256", "ES256"])
    s.add_argument("--kid")
    s.add_argument("--iss", default="http://verifier.ft")
    s.add_argument("--aud", default="data-service")
    s.add_argument("--exp", type=int, default=1800)
    s.add_argument("--vc")
    s.add_argument("--vp")
    s.add_argument("--claims")
    s.add_argument("--badSig", action="store_true")

    a = p.parse_args()
    {"key": cmdKey, "jwks": cmdJwks, "sign": cmdSign}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
