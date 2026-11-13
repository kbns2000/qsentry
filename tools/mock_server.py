#!/usr/bin/env python3
"""A stand-in for o6q_server, to test scripts/o6q_client.py without QNX.

    python tools/mock_server.py --selftest
    python tools/mock_server.py --port 48401 --cert C.der --key K.pem
                                --trust DIR --user NAME:PASSWORD [--seconds 120]

python asyncua plays the server, set up like src/o6q_server.c: the namespace
urn:open62541-qnx:system, the object QNX with string node ids "QNX.<name>",
SignAndEncrypt endpoints plus None for discovery, a user name and password
(no anonymous sessions) and only the client certificates in DIR are trusted.
The values are made up (OS says "QNX" so that the client runs every check).
Nothing this program prints is a measurement of QNX.

--selftest makes the certificates in a temporary directory, starts the
server, runs scripts/o6q_client.py against it and exits with its status.
Needs: pip install asyncua
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import asyncio
import os
import random
import shutil
import socket
import sys
import tempfile
import time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(os.path.dirname(HERE), "scripts")
sys.path.insert(0, SCRIPTS)
import o6q_client  # noqa: E402  (make_cert, NAMESPACE_URI)

SERVER_URI = "urn:open62541-qnx:mock-server"


async def serve(port, cert, key, trust, user, seconds, ready=None):
    from asyncua import Server, ua
    from asyncua.crypto.permission_rules import User, UserRole
    from asyncua.crypto.truststore import TrustStore
    from asyncua.crypto.validator import CertificateValidator, CertificateValidatorOptions

    login = tuple(user.split(":", 1))

    class Users:
        def get_user(self, iserver, username=None, password=None, certificate=None):
            if (username, password) == login:
                return User(role=UserRole.User)
            return None

    server = Server(user_manager=Users())
    await server.init()
    await server.set_application_uri(SERVER_URI)
    server.set_endpoint(f"opc.tcp://127.0.0.1:{port}/")
    server.set_server_name("o6q mock server (python asyncua)")
    P = ua.SecurityPolicyType
    server.set_security_policy([P.NoSecurity, P.Basic256Sha256_SignAndEncrypt,
                                P.Aes128Sha256RsaOaep_SignAndEncrypt,
                                P.Aes256Sha256RsaPss_SignAndEncrypt])
    server.set_identity_tokens([ua.UserNameIdentityToken])
    await server.load_certificate(cert)
    await server.load_private_key(key)
    store = TrustStore([Path(trust)], [])
    await store.load()
    server.set_certificate_validator(CertificateValidator(
        CertificateValidatorOptions.TRUSTED_VALIDATION | CertificateValidatorOptions.PEER_CLIENT,
        store))

    idx = await server.register_namespace(o6q_client.NAMESPACE_URI)
    qnx = await server.nodes.objects.add_object(ua.NodeId("QNX", idx), f"{idx}:QNX")
    V = ua.VariantType
    initial = [("OS", "QNX", V.String), ("Release", "mock", V.String),
               ("Machine", "mock", V.String), ("Hostname", socket.gethostname(), V.String),
               ("CpuCount", 2, V.UInt32), ("CyclesPerSecond", 1000000000, V.UInt64),
               ("ClockPeriodNs", 1000000, V.UInt32), ("RamTotalMB", 1024.0, V.Double),
               ("Uptime", 0.0, V.Double), ("RamFreeMB", 900.0, V.Double),
               ("Processes", 20, V.UInt32), ("Threads", 90, V.UInt32),
               ("CpuLoad", [0.0, 0.0], V.Double), ("CpuLoadAverage", 0.0, V.Double),
               ("ProcessList", ["1 procnto (mock)"], V.String),
               ("OpcUaSessions", 0, V.UInt32), ("Stack", "python asyncua (mock)", V.String)]
    nodes = {}
    for name, value, vtype in initial:
        nodes[name] = await qnx.add_variable(ua.NodeId(f"QNX.{name}", idx), f"{idx}:{name}",
                                             ua.Variant(value, vtype))

    async with server:
        print(f"MOCK READY opc.tcp://127.0.0.1:{port} asyncua server", flush=True)
        if ready:
            ready.set()
        t0 = time.monotonic()
        while time.monotonic() - t0 < seconds:
            await asyncio.sleep(1.0)
            load = random.uniform(5, 30)
            await nodes["Uptime"].write_value(ua.Variant(time.monotonic() - t0, V.Double))
            await nodes["CpuLoadAverage"].write_value(ua.Variant(load, V.Double))
            await nodes["CpuLoad"].write_value(ua.Variant([load, load], V.Double))
        print("MOCK STOP", flush=True)


async def selftest():
    tmp = tempfile.mkdtemp(prefix="o6q_mock_")
    try:
        pki = Path(tmp)
        (pki / "trusted").mkdir()
        o6q_client.make_cert(pki / "server.der", pki / "server.pem", "o6q mock server",
                             SERVER_URI)
        o6q_client.make_cert(pki / "client.der", pki / "client.pem")
        shutil.copy(pki / "client.der", pki / "trusted" / "client.der")
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        ready = asyncio.Event()
        task = asyncio.create_task(serve(port, str(pki / "server.der"), str(pki / "server.pem"),
                                         str(pki / "trusted"), "operator:mock-pw", 600, ready))
        waiter = asyncio.create_task(ready.wait())
        await asyncio.wait({task, waiter}, timeout=60, return_when=asyncio.FIRST_COMPLETED)
        if task.done():
            task.result()  # the server failed to start: raise its error
        if not ready.is_set():
            raise RuntimeError("the mock server did not start within 60 s")
        proc = await asyncio.create_subprocess_exec(
            sys.executable, os.path.join(SCRIPTS, "o6q_client.py"), f"opc.tcp://127.0.0.1:{port}",
            "--cert", str(pki / "client.der"), "--key", str(pki / "client.pem"),
            "--user", "operator:mock-pw", "--seconds", "6")
        code = await proc.wait()
        task.cancel()
        try:
            await task
        except (asyncio.CancelledError, Exception):  # noqa: BLE001
            pass
        print(f"MOCK SELFTEST client exit {code}", flush=True)
        return code
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--port", type=int, default=48401)
    ap.add_argument("--cert")
    ap.add_argument("--key")
    ap.add_argument("--trust")
    ap.add_argument("--user", default="operator:mock-pw")
    ap.add_argument("--seconds", type=int, default=120)
    args = ap.parse_args()
    if args.selftest:
        return asyncio.run(selftest())
    if not (args.cert and args.key and args.trust):
        ap.error("--cert, --key and --trust are needed (or --selftest)")
    asyncio.run(serve(args.port, args.cert, args.key, args.trust, args.user, args.seconds))
    return 0


if __name__ == "__main__":
    sys.exit(main())
