"""Keep this Mac's preview signing identity stable; never ship its private key."""
import ctypes
import datetime
import hashlib
import os
from pathlib import Path
import secrets
import shlex
import subprocess
import tempfile


def identity():
    folder=Path.home()/'Library/Application Support/AI Passport/signing'
    folder.mkdir(parents=True,exist_ok=True,mode=0o700);folder.chmod(0o700)
    keychain=folder/'development.keychain-db';certificate=folder/'certificate.der';password_file=folder/'keychain.password'
    security=ctypes.CDLL('/System/Library/Frameworks/Security.framework/Security')
    reference=ctypes.c_void_p()
    security.SecKeychainCreate.argtypes=[ctypes.c_char_p,ctypes.c_uint32,ctypes.c_char_p,ctypes.c_bool,ctypes.c_void_p,ctypes.POINTER(ctypes.c_void_p)]
    security.SecKeychainOpen.argtypes=[ctypes.c_char_p,ctypes.POINTER(ctypes.c_void_p)]
    security.SecKeychainUnlock.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_char_p,ctypes.c_bool]
    if not any(p.exists() for p in (keychain,certificate,password_file)):
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes,serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography.x509.oid import NameOID,ExtendedKeyUsageOID
        key=rsa.generate_private_key(public_exponent=65537,key_size=3072)
        name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'AI Passport Local Development')])
        now=datetime.datetime.now(datetime.timezone.utc)
        cert=(x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
              .serial_number(x509.random_serial_number()).not_valid_before(now-datetime.timedelta(minutes=5))
              .not_valid_after(now+datetime.timedelta(days=3650))
              .add_extension(x509.BasicConstraints(ca=False,path_length=None),critical=True)
              .add_extension(x509.KeyUsage(True,False,False,False,False,False,False,False,False),critical=True)
              .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CODE_SIGNING]),critical=True)
              .sign(key,hashes.SHA256()))
        password=secrets.token_hex(32).encode()
        with os.fdopen(os.open(password_file,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'wb') as target:target.write(password)
        code=security.SecKeychainCreate(os.fsencode(keychain),len(password),password,False,None,ctypes.byref(reference))
        if code:raise RuntimeError('Cannot create local signing keychain: '+str(code))
        with tempfile.TemporaryDirectory(prefix='passport-signing-import-') as temp:
            archive=Path(temp)/'private-key.pem'
            with os.fdopen(os.open(archive,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'wb') as target:
                target.write(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.TraditionalOpenSSL,serialization.NoEncryption()))
            certificate.write_bytes(cert.public_bytes(serialization.Encoding.DER));certificate.chmod(0o600)
            for item in (archive,certificate):
                result=subprocess.run(['/usr/bin/security','import',str(item),'-k',str(keychain),'-T','/usr/bin/codesign'],capture_output=True,text=True)
                if result.returncode:raise RuntimeError('Local signing identity import failed: '+result.stderr.strip())
    if not all(p.is_file() for p in (keychain,certificate,password_file)):
        raise RuntimeError('Local signing identity is incomplete; restore it instead of creating a new identity')
    if not reference.value:
        code=security.SecKeychainOpen(os.fsencode(keychain),ctypes.byref(reference))
        if code:raise RuntimeError('Cannot open local signing keychain: '+str(code))
    password=password_file.read_bytes()
    code=security.SecKeychainUnlock(reference,len(password),password,True)
    if code:raise RuntimeError('Cannot unlock local signing keychain: '+str(code))
    return hashlib.sha1(certificate.read_bytes()).hexdigest().upper(),keychain


def sign(path,signer):
    fingerprint,keychain=signer
    previous=shlex.split(subprocess.check_output(['/usr/bin/security','list-keychains','-d','user'],text=True))
    try:
        subprocess.run(['/usr/bin/security','list-keychains','-d','user','-s',*previous,str(keychain)],check=True)
        subprocess.run(['/usr/bin/codesign','--force','--deep','--timestamp=none','--keychain',str(keychain),'--sign',fingerprint,str(path)],check=True)
    finally:subprocess.run(['/usr/bin/security','list-keychains','-d','user','-s',*previous],check=True)


def requirement(path):
    result=subprocess.run(['/usr/bin/codesign','-d','-r-',str(path)],capture_output=True,text=True,check=True)
    text=result.stdout+'\n'+result.stderr
    return next(line.split('designated => ',1)[1] for line in text.splitlines() if 'designated => ' in line)
