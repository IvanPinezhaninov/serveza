/******************************************************************************
**
** Copyright (C) 2026 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
**
** This file is part of the serveza - which can be found at
** https://github.com/IvanPinezhaninov/serveza/.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
** DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
** OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
** THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
******************************************************************************/

#include <iostream>
#include <random>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/types.h>
#include <openssl/x509v3.h>

#include "serveza/tls_cert_utils.h"

namespace serveza::tls_cert_utils {

void generate_key_and_cert(const std::filesystem::path& certPath, const std::filesystem::path& keyPath)
{
  namespace fs = std::filesystem;

  if (fs::exists(certPath) && fs::exists(keyPath)) return;

  fs::create_directories(certPath.parent_path());
  fs::create_directories(keyPath.parent_path());

  fs::remove(certPath);
  fs::remove(keyPath);

  std::cout << "Generating certificate and key..." << std::endl;

  using EVP_PKEY_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
  using X509_ptr = std::unique_ptr<X509, decltype(&X509_free)>;
  using EVP_PKEY_CTX_ptr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;

  static constexpr int bits = 2048;
  static constexpr int daysValid = 365;

  EVP_PKEY_CTX_ptr keyCtx{EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), &EVP_PKEY_CTX_free};
  if (!keyCtx) throw std::runtime_error{"Failed to create EVP_PKEY_CTX"};
  if (EVP_PKEY_keygen_init(keyCtx.get()) <= 0) throw std::runtime_error{"Failed to init keygen"};
  if (EVP_PKEY_CTX_set_rsa_keygen_bits(keyCtx.get(), bits) <= 0) throw std::runtime_error{"Failed to set key bits"};

  EVP_PKEY* rawPkey = nullptr;
  if (EVP_PKEY_keygen(keyCtx.get(), &rawPkey) <= 0) throw std::runtime_error{"Failed to generate key"};
  EVP_PKEY_ptr pkey{rawPkey, &EVP_PKEY_free};

  X509_ptr x509{X509_new(), &X509_free};
  if (!x509) throw std::runtime_error{"Failed to create X509 object"};

  auto setX509RandomSerial = [](X509* cert) {
    std::random_device rd;
    std::uint32_t v = (std::uint32_t(rd()) << 16) ^ std::uint32_t(rd());
    if (v == 0) v = 1;

    ASN1_INTEGER* serial = X509_get_serialNumber(cert);
    if (serial == nullptr) throw std::runtime_error{"Failed to get serial number"};

    ASN1_INTEGER_set(serial, static_cast<long>(v));
  };

  X509_set_version(x509.get(), 2);
  setX509RandomSerial(x509.get());

  X509_gmtime_adj(X509_get_notBefore(x509.get()), 0);
  X509_gmtime_adj(X509_get_notAfter(x509.get()), 60L * 60L * 24L * daysValid);

  if (X509_set_pubkey(x509.get(), pkey.get()) != 1)
    throw std::runtime_error{"Failed to assign public key to certificate"};

  X509_NAME* name = X509_get_subject_name(x509.get());
  if (!name) throw std::runtime_error{"Failed to get subject name"};

  X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (const unsigned char*)"XX", -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char*)"MyOrg", -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)"localhost", -1, -1, 0);

  if (X509_set_issuer_name(x509.get(), name) != 1) throw std::runtime_error{"Failed to set issuer name"};

  auto addX509Ext = [](X509* cert, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);

    X509_EXTENSION* ex = X509V3_EXT_nconf_nid(nullptr, &ctx, nid, const_cast<char*>(value));
    if (!ex) throw std::runtime_error{"Failed to create X509 extension"};
    if (X509_add_ext(cert, ex, -1) != 1) {
      X509_EXTENSION_free(ex);
      throw std::runtime_error{"Failed to add X509 extension"};
    }
    X509_EXTENSION_free(ex);
  };

  addX509Ext(x509.get(), NID_basic_constraints, "CA:FALSE");
  addX509Ext(x509.get(), NID_key_usage, "digitalSignature,keyEncipherment");
  addX509Ext(x509.get(), NID_ext_key_usage, "serverAuth");
  addX509Ext(x509.get(), NID_subject_key_identifier, "hash");
  addX509Ext(x509.get(), NID_authority_key_identifier, "keyid");
  addX509Ext(x509.get(), NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");

  if (!X509_sign(x509.get(), pkey.get(), EVP_sha256())) throw std::runtime_error{"Failed to sign the certificate"};

  struct FileCloser {
    void operator()(FILE* file) const noexcept
    {
      if (file != nullptr) std::fclose(file);
    }
  };

  {
    std::unique_ptr<FILE, FileCloser> keyFile{std::fopen(keyPath.string().c_str(), "wb")};
    if (!keyFile) throw std::runtime_error{"Failed to open key file for writing"};
    if (!PEM_write_PrivateKey(keyFile.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr))
      throw std::runtime_error{"Failed to write private key"};
  }

  {
    std::unique_ptr<FILE, FileCloser> certFile{std::fopen(certPath.string().c_str(), "wb")};
    if (!certFile) throw std::runtime_error{"Failed to open cert file for writing"};
    if (!PEM_write_X509(certFile.get(), x509.get())) throw std::runtime_error{"Failed to write certificate"};
  }

  std::cout << "Successfully generated: " << certPath << " and " << keyPath << std::endl;
}

} // namespace serveza::tls_cert_utils
