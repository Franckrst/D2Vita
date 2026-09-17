/* tools/wtref/wtref.c — Win32 reference leg of the Authenticode oracle
 * (WinVerifyTrust + CRYPT32), run under Wine with no network (unshare -rn).
 *
 * For each file given as an argument, replays exactly the calls made by the
 * two real callers, with arguments taken from disassembly:
 *   - Game.exe 1.14d, at 0x516de1 -> 0x517ad6 (WinVerifyTrust) then
 *     0x517c30 (CryptQueryObject ... CertGetNameStringW "2.5.4.10");
 *   - CheckRevision.dll 1.14d, 0x10001f4b (WinVerifyTrust) then 0x10001fe4
 *     (CryptQueryObject ... CertFindCertificateInStore, public key compared).
 * Prints a canonical dump (no pointers, 64-bit FNV-1a hashes of each blob)
 * that tools/wtref/wtref_host.cpp reproduces identically against our shims.
 * Compared by tools/oracle_authenticode.sh.
 *
 * Build: i686-w64-mingw32-gcc -O2 -o wtref.exe wtref.c -lwintrust -lcrypt32
 */
#include <windows.h>
#include <wintrust.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>

static unsigned long long fnv(const BYTE* p, DWORD n) {
    unsigned long long h = 1469598103934665603ULL;
    DWORD i; for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
static void hex(const BYTE* p, DWORD n) { DWORD i; for (i = 0; i < n; i++) printf("%02x", p[i]); }
static void blob(const char* tag, const BYTE* p, DWORD n) {
    printf(" %s=%lu:%016llx", tag, (unsigned long)n, p && n ? fnv(p, n) : 0ULL);
}
static void alg(const char* tag, const CRYPT_ALGORITHM_IDENTIFIER* a) {
    printf(" %s=%s", tag, a->pszObjId ? a->pszObjId : "(null)");
    blob("par", a->Parameters.pbData, a->Parameters.cbData);
}
static void attrs(const char* tag, const CRYPT_ATTRIBUTES* a) {
    DWORD i, j; printf("SI %s n=%lu\n", tag, (unsigned long)a->cAttr);
    for (i = 0; i < a->cAttr; i++) {
        printf("SI %s[%lu] %s vals=%lu", tag, (unsigned long)i, a->rgAttr[i].pszObjId, (unsigned long)a->rgAttr[i].cValue);
        for (j = 0; j < a->rgAttr[i].cValue; j++) blob("v", a->rgAttr[i].rgValue[j].pbData, a->rgAttr[i].rgValue[j].cbData);
        printf("\n");
    }
}
static void utf8(const WCHAR* w) {
    for (; *w; w++) { if (*w >= 32 && *w < 127) putchar((char)*w); else printf("\\u%04x", *w); }
}

/* WINTRUST_DATA for both callers, field by field, from disassembly. */
static void wvt(const WCHAR* path, int crStyle) {
    GUID act = { 0x00AAC56B, 0xCD44, 0x11D0, { 0x8C, 0xC2, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE } }; /* WINTRUST_ACTION_GENERIC_VERIFY_V2 */
    WINTRUST_FILE_INFO fi; WINTRUST_DATA wd; LONG r;
    memset(&fi, 0, sizeof fi); memset(&wd, 0, sizeof wd);
    fi.cbStruct = 0x10; fi.pcwszFilePath = path;
    wd.cbStruct = 0x30; wd.dwUIChoice = WTD_UI_NONE; wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE; wd.pFile = &fi; wd.dwStateAction = 0;
    wd.dwProvFlags = crStyle ? 0x100 : 0;
    SetLastError(0xdeadbeef);
    r = WinVerifyTrust(crStyle ? (HWND)INVALID_HANDLE_VALUE : NULL, &act, &wd);
    printf("WVT %s hr=0x%08lx le=0x%08lx\n", crStyle ? "checkrevision" : "game", (unsigned long)r, (unsigned long)GetLastError());
}

static void run(const WCHAR* path) {
    HCERTSTORE st = NULL; HCRYPTMSG msg = NULL; BOOL ok; DWORD cb = 0, n, i;
    CMSG_SIGNER_INFO* si; CERT_INFO ci; PCCERT_CONTEXT ctx;
    wvt(path, 0);
    wvt(path, 1);
    SetLastError(0xdeadbeef);
    ok = CryptQueryObject(CERT_QUERY_OBJECT_FILE, path, CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0, NULL, NULL, NULL, &st, &msg, NULL);
    printf("CQO ret=%d le=0x%08lx store=%d msg=%d\n", ok, (unsigned long)(ok ? 0 : GetLastError()), st != NULL, msg != NULL);
    if (!ok) return;
    { DWORD enc = 0, ct = 0, fmt = 0; HCERTSTORE s2 = NULL; HCRYPTMSG m2 = NULL;
      ok = CryptQueryObject(CERT_QUERY_OBJECT_FILE, path, CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                            CERT_QUERY_FORMAT_FLAG_BINARY, 0, &enc, &ct, &fmt, &s2, &m2, NULL);
      printf("CQO2 ret=%d enc=0x%lx ct=%lu fmt=%lu\n", ok, (unsigned long)enc, (unsigned long)ct, (unsigned long)fmt);
      if (m2) CryptMsgClose(m2); if (s2) CertCloseStore(s2, 0); }
    cb = sizeof n; ok = CryptMsgGetParam(msg, CMSG_SIGNER_COUNT_PARAM, 0, &n, &cb);
    printf("MSG signers ret=%d n=%lu\n", ok, (unsigned long)n);
    cb = sizeof n; ok = CryptMsgGetParam(msg, CMSG_CERT_COUNT_PARAM, 0, &n, &cb);
    printf("MSG certs ret=%d n=%lu\n", ok, (unsigned long)n);
    for (i = 0; i < n; i++) {
        BYTE buf[8192]; DWORD c2 = sizeof buf;
        ok = CryptMsgGetParam(msg, CMSG_CERT_PARAM, i, buf, &c2);
        printf("MSG cert[%lu] ret=%d", (unsigned long)i, ok); blob("der", buf, ok ? c2 : 0); printf("\n");
    }
    SetLastError(0xdeadbeef);
    ok = CryptMsgGetParam(msg, 99, 0, NULL, &cb);
    printf("MSG param99 ret=%d le=0x%08lx\n", ok, (unsigned long)(ok ? 0 : GetLastError()));
    SetLastError(0xdeadbeef);
    ok = CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 5, NULL, &cb);
    printf("MSG signer5 ret=%d le=0x%08lx\n", ok, (unsigned long)(ok ? 0 : GetLastError()));

    /* ---- caller sequence ---- */
    cb = 0; ok = CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &cb);
    printf("SI size ret=%d\n", ok);            /* the size itself depends on layout: not compared */
    si = (CMSG_SIGNER_INFO*)LocalAlloc(LMEM_ZEROINIT, cb);
    { DWORD small = 8; SetLastError(0xdeadbeef);
      ok = CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, si, &small);
      printf("SI small ret=%d le=0x%08lx grown=%d\n", ok, (unsigned long)(ok ? 0 : GetLastError()), small == cb); }
    ok = CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, si, &cb);
    printf("SI ret=%d ver=%lu", ok, (unsigned long)si->dwVersion);
    blob("issuer", si->Issuer.pbData, si->Issuer.cbData);
    printf(" serial="); hex(si->SerialNumber.pbData, si->SerialNumber.cbData);
    alg("hash", &si->HashAlgorithm); alg("enc", &si->HashEncryptionAlgorithm);
    blob("encdig", si->EncryptedHash.pbData, si->EncryptedHash.cbData);
    printf("\n");
    attrs("auth", &si->AuthAttrs); attrs("unauth", &si->UnauthAttrs);

    memset(&ci, 0, sizeof ci);
    ci.Issuer = si->Issuer; ci.SerialNumber = si->SerialNumber;
    SetLastError(0xdeadbeef);
    ctx = CertFindCertificateInStore(st, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &ci, NULL);
    printf("FIND ret=%d le=0x%08lx\n", ctx != NULL, (unsigned long)(ctx ? 0 : GetLastError()));
    if (ctx) {
        CERT_INFO* x = ctx->pCertInfo; DWORD k; WCHAR name[512]; DWORD nn;
        printf("CTX enc=%lu", (unsigned long)ctx->dwCertEncodingType); blob("der", ctx->pbCertEncoded, ctx->cbCertEncoded);
        printf(" storeMatch=%d\n", ctx->hCertStore == st);
        printf("CI ver=%lu serial=", (unsigned long)x->dwVersion); hex(x->SerialNumber.pbData, x->SerialNumber.cbData);
        alg("sig", &x->SignatureAlgorithm); blob("issuer", x->Issuer.pbData, x->Issuer.cbData);
        printf(" nb=%08lx%08lx na=%08lx%08lx", (unsigned long)x->NotBefore.dwHighDateTime, (unsigned long)x->NotBefore.dwLowDateTime,
               (unsigned long)x->NotAfter.dwHighDateTime, (unsigned long)x->NotAfter.dwLowDateTime);
        blob("subject", x->Subject.pbData, x->Subject.cbData);
        alg("spki", &x->SubjectPublicKeyInfo.Algorithm);
        blob("pk", x->SubjectPublicKeyInfo.PublicKey.pbData, x->SubjectPublicKeyInfo.PublicKey.cbData);
        printf(" unused=%lu pkhead=", (unsigned long)x->SubjectPublicKeyInfo.PublicKey.cUnusedBits);
        hex(x->SubjectPublicKeyInfo.PublicKey.pbData, x->SubjectPublicKeyInfo.PublicKey.cbData > 12 ? 12 : x->SubjectPublicKeyInfo.PublicKey.cbData);
        printf(" ext=%lu\n", (unsigned long)x->cExtension);
        for (k = 0; k < x->cExtension; k++) {
            printf("CI ext[%lu] %s crit=%d", (unsigned long)k, x->rgExtension[k].pszObjId, x->rgExtension[k].fCritical);
            blob("val", x->rgExtension[k].Value.pbData, x->rgExtension[k].Value.cbData); printf("\n");
        }
        nn = CertGetNameStringW(ctx, CERT_NAME_ATTR_TYPE, 0, (LPVOID)"2.5.4.10", NULL, 0);
        printf("NAME O size=%lu", (unsigned long)nn);
        nn = CertGetNameStringW(ctx, CERT_NAME_ATTR_TYPE, 0, (LPVOID)"2.5.4.10", name, 512);
        printf(" ret=%lu \"", (unsigned long)nn); utf8(name); printf("\"\n");
        nn = CertGetNameStringW(ctx, CERT_NAME_ATTR_TYPE, 0, (LPVOID)"2.5.4.10", name, 9);
        printf("NAME O trunc ret=%lu \"", (unsigned long)nn); utf8(name); printf("\"\n");
        nn = CertGetNameStringW(ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, 512);
        printf("NAME simple ret=%lu \"", (unsigned long)nn); utf8(name); printf("\"\n");
        nn = CertGetNameStringW(ctx, CERT_NAME_ATTR_TYPE, CERT_NAME_ISSUER_FLAG, (LPVOID)"2.5.4.3", name, 512);
        printf("NAME issuerCN ret=%lu \"", (unsigned long)nn); utf8(name); printf("\"\n");
        nn = CertGetNameStringW(ctx, CERT_NAME_ATTR_TYPE, 0, (LPVOID)"2.5.4.12", name, 512);
        printf("NAME absent ret=%lu \"", (unsigned long)nn); utf8(name); printf("\"\n");
        printf("FREE ret=%d\n", CertFreeCertificateContext(ctx));
    }
    LocalFree(si);
    printf("CLOSE store=%d msg=%d\n", CertCloseStore(st, 0), CryptMsgClose(msg));
}

int wmain(int argc, WCHAR** argv) {
    int i;
    for (i = 1; i < argc; i++) {
        const WCHAR* base = wcsrchr(argv[i], L'\\'); base = base ? base + 1 : argv[i];
        printf("== "); utf8(base); printf("\n");
        run(argv[i]);
    }
    return 0;
}
