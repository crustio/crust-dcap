#include "sgx_report.h"
#include "sgx_dcap_ql_wrapper.h"
#include "sgx_pce.h"
#include "sgx_error.h"
#include "sgx_quote_3.h"
#include "sgx_ql_quote.h"
#include "sgx_dcap_quoteverify.h"
#include "sgx_utils.h"
#include "sgx_urts.h"
#include "sgx_tcrypto.h"
#include <sgx_ecp_types.h>

#include "httplib.h"
#include "Json.h"
#include "Log.h"
#include "Defer.h"
#include "Utils.h"

extern "C" {
#include "sr25519.h"
}

using namespace httplib;

std::string host = "0.0.0.0";
int port = 1234;
std::string seed;
std::vector<uint8_t> kp(SR25519_KEYPAIR_SIZE, 0);
std::string dcap_pubkey;

int show_help(const char *name)
{
    printf("    Usage: \n");
    printf("        %s <option> <argument>\n", name);
    printf("          option: \n");
    printf("           -h, --help: help information. \n");
    printf("           -t, --host: set server host, default is %s \n", host.c_str());
    printf("           -p, --port: set server port, default is %d \n", port);
    printf("           -s, --seed: set sr25519 scheme secret seed for signing\n");

    return 1;
}

int main(int argc, char *argv[])
{
    Log *p_log = Log::get_instance();

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            return show_help(argv[0]);
        }
        else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--host") == 0)
        {
            if (i + 1 >= argc)
            {
                p_log->err("-t,--host option needs a hostname as argument!\n");
                return 1;
            }
            i++;
            host = argv[i];
        }
        else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0)
        {
            if (i + 1 >= argc)
            {
                p_log->err("-p,--port option needs a port number as argument!\n");
                return 1;
            }
            i++;
            port = std::atoi(argv[i]);
        }
        else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--seed") == 0)
        {
            if (i + 1 >= argc)
            {
                p_log->err("-s,--seed option needs a seed string as argument!\n");
                return 1;
            }
            i++;
            seed = argv[i];
            if (seed.size() != 64)
            {
                p_log->err("Invalid seed string, seed string size must be 64!\n");
                return 1;
            }
        }
        else
        {
            return show_help(argv[0]);
        }
    }

    if (seed.size() == 0) 
    {
        p_log->err("Must specify a seed string in the argument!\n");
        return 1;
    }

    // Generate sr25519 keypair from the specified seed
    sr25519_keypair_from_seed(kp.data(), hexstring_to_bytes(seed.c_str(), seed.size()));
    std::string dcap_pubkey = hexstring(kp.data()+SR25519_SECRET_SIZE, SR25519_PUBLIC_SIZE);

    p_log->info("Start dcap service at %s:%d successfully!\n", host.c_str(), port);
    Server svr;

    svr.Get("/hello", [](const Request& /*req*/, Response& res) {
        res.set_content("Hello World!", "text/plain");
    });

    svr.Get("/stop", [&](const Request& /*req*/, Response& /*res*/) {
        svr.stop();
    });

    svr.Post("/attestation/report", [&](const Request& req, Response& res) {
        //call DCAP quote verify library to verify the quote
        p_log->info("Dealing with new attestation request...\n");
        p_log->info("Request body:%s\n",req.body.c_str());

	    int ret = 0;
        json::JSON ret_body;
        json::JSON report_body;
        sgx_ql_qv_result_t quote_verification_result = SGX_QL_QV_RESULT_UNSPECIFIED;
	    uint32_t collateral_expiration_status = 1;
        uint32_t supplemental_data_size = 0;
        uint8_t *p_supplemental_data = NULL;

        Defer def_ret([&res, &ret_body, &report_body, p_log](void) {
            int status = ret_body["status_code"].ToInt();
            // Provide the report body only if the quote verification success
            if (200 == status)
            {
                ret_body["report_body"] = json::Array();
                ret_body["report_body"].append(report_body);
            }
            res.status = status;
            std::string body = ret_body.dump();
            remove_char(body, '\\');
            remove_char(body, '\n');
            res.set_content(body, "application/json");

            p_log->info("Response body: %s\n", body.c_str());
        });

        crust_status_t crust_status = CRUST_SUCCESS;
        json::JSON ecdsa_identity = json::JSON::Load(&crust_status, req.body);
        if (CRUST_SUCCESS != crust_status)
        {
            p_log->err("Load ecdsa_identity failed! Error code:%x\n", crust_status);
            ret_body["message"] = "Load ecdsa_identity failed!";
            ret_body["status_code"] = 400;
            return;
        }

        // ----- Verify signature ----- //
        std::string sig = ecdsa_identity["sig"].ToString();
        uint8_t *p_sig = hexstring_to_bytes(sig.c_str(), sig.size());
        if (p_sig == NULL)
        {
            ret_body["message"] = "Unexpected error";
            ret_body["status_code"] = 400;
            return ;
        }
        Defer def_sig([&p_sig](void) { free(p_sig); });
        uint8_t p_result;
        // Get signature data
        std::string quote_hexstr = ecdsa_identity["quote"].ToString();
        uint8_t *p_quote = hexstring_to_bytes(quote_hexstr.c_str(), quote_hexstr.size());
        uint32_t quote_sz = quote_hexstr.size() / 2;
        std::string account_id = ecdsa_identity["account"].ToString();
        uint32_t sig_data_sz = quote_sz + account_id.size();
        uint8_t *p_sig_data = (uint8_t *)malloc(sig_data_sz);
        Defer def_sig_data([&p_sig_data](void) { free(p_sig_data); });
        memset(p_sig_data, 0, sig_data_sz);
        memcpy(p_sig_data, p_quote, quote_sz);
        memcpy(p_sig_data + quote_sz, account_id.c_str(), account_id.size());
        _sgx_quote3_t *quote = (_sgx_quote3_t*)p_quote;
        uint8_t *p_pub_key = reinterpret_cast<uint8_t *>(&quote->report_body.report_data);
        uint8_t *p_mr_enclave = reinterpret_cast<uint8_t *>(&quote->report_body.mr_enclave);
        uint32_t identity_sz = sizeof(sgx_report_data_t) + sizeof(sgx_measurement_t) + account_id.size();
        
        // Verify signature
        sgx_sha256_hash_t msg_hash;
        sgx_sha256_msg(p_sig_data, sig_data_sz, &msg_hash);
        EC_KEY *ec_pkey = key_from_sgx_ec256((sgx_ec256_public_t *)p_pub_key);
        ret = ECDSA_verify(0, reinterpret_cast<const uint8_t *>(&msg_hash), sizeof(sgx_sha256_hash_t),
                p_sig, sizeof(sgx_ec256_signature_t), ec_pkey);
        if (0 == ret)
        {
            ret_body["message"] = "Verify identity signature failed!";
            ret_body["status_code"] = 500;
            return;
        }

        // ----- Verify qutoe ----- //
        quote3_error_t dcap_ret = sgx_qv_get_quote_supplemental_data_size(&supplemental_data_size);
        if (dcap_ret == SGX_QL_SUCCESS && supplemental_data_size == sizeof(sgx_ql_qv_supplemental_t)) 
        {
            p_log->info("sgx_qv_get_quote_supplemental_data_size successfully returned.\n");
            p_supplemental_data = (uint8_t*)malloc(supplemental_data_size);
        }
        else {
            p_log->err("sgx_qv_get_quote_supplemental_data_size failed: 0x%04x\n", dcap_ret);
            supplemental_data_size = 0;
        }

        //set current time. This is only for sample purposes, in production mode a trusted time should be used.
        time_t current_time = time(NULL);


        //call DCAP quote verify library for quote verification
        //here you can choose 'trusted' or 'untrusted' quote verification by specifying parameter '&qve_report_info'
        //if '&qve_report_info' is NOT NULL, this API will call Intel QvE to verify quote
        //if '&qve_report_info' is NULL, this API will call 'untrusted quote verify lib' to verify quote, this mode doesn't rely on SGX capable system, but the results can not be cryptographically authenticated
        dcap_ret = sgx_qv_verify_quote(
            p_quote, (uint32_t)quote_sz,
            NULL,
            current_time,
            &collateral_expiration_status,
            &quote_verification_result,
            NULL,
            supplemental_data_size,
            p_supplemental_data);
        if (p_supplemental_data != NULL)
            free(p_supplemental_data);
        if (dcap_ret == SGX_QL_SUCCESS)
        {
            p_log->info("App: sgx_qv_verify_quote successfully returned.\n");
        }
        else
        {
            switch (dcap_ret)
            {
            case SGX_QL_QUOTE_FORMAT_UNSUPPORTED:
                p_log->err("The inputted quote format is not supported. Either because the header information is not supported or the quote is malformed in some way.\n");
                break;
            case SGX_QL_QUOTE_CERTIFICATION_DATA_UNSUPPORTED:
                p_log->err("The quote verifier doesn’t support the certification data in the Quote. Currently, the Intel QVE only supported CertType = 5.\n");
                break;
            case SGX_QL_QE_REPORT_UNSUPPORTED_FORMAT:
                p_log->err("The quote verifier doesn’t support the format of the application REPORT the Quote.\n");
                break;
            case SGX_QL_QE_REPORT_INVALID_SIGNATURE:
                p_log->err("The signature over the QE Report is invalid.\n");
                break;
            case SGX_QL_PCK_CERT_UNSUPPORTED_FORMAT:
                p_log->err("The format of the PCK Cert is unsupported.\n");
                break;
            case SGX_QL_PCK_CERT_CHAIN_ERROR:
                p_log->err("There was an error verifying the PCK Cert signature chain including PCK Cert revocation.\n");
                break;
            case SGX_QL_TCBINFO_UNSUPPORTED_FORMAT:
                p_log->err("The format of the TCBInfo structure is unsupported.\n");
                break;
            case SGX_QL_TCBINFO_CHAIN_ERROR:
                p_log->err("There was an error verifying the TCBInfo signature chain including TCBInfo revocation.\n");
                break;
            case SGX_QL_TCBINFO_MISMATCH:
                p_log->err("PCK Cert FMSPc does not match the TCBInfo FMSPc.\n");
                break;
            case SGX_QL_QEIDENTITY_UNSUPPORTED_FORMAT:
                p_log->err("The format of the QEIdentity structure is unsupported.\n");
                break;
            case SGX_QL_QEIDENTITY_MISMATCH:
                p_log->err("The Quote’s QE doesn’t match the inputted expected QEIdentity.\n");
                break;
            case SGX_QL_QEIDENTITY_CHAIN_ERROR:
                p_log->err("There was an error verifying the QEIdentity signature chain including QEIdentity revocation.\n");
                break;
            case SGX_QL_ENCLAVE_LOAD_ERROR:
                p_log->err("Unable to load the enclaves required to initialize the attestation key. error, loading infrastructure error or insufficient enclave memory.\n");
                break;
            case SGX_QL_ENCLAVE_LOST:
                p_log->err("Could be due to file I/O. Enclave lost after power transition or used in child process created by linux:fork().\n");
                break;
            case SGX_QL_INVALID_REPORT:
                p_log->err("Report MAC check failed on application report.\n");
                break;
            case SGX_QL_PLATFORM_LIB_UNAVAILABLE:
                p_log->err("The Quote Library could not locate the platform quote provider library or one of its required APIs.\n");
                break;
            case SGX_QL_UNABLE_TO_GENERATE_REPORT:
                p_log->err("The QVE was unable to generate its own report targeting the application enclave because there is an enclave compatibility issue.\n");
                break;
            case SGX_QL_NETWORK_ERROR:
                p_log->err("Network error when retrieving PCK certs.\n");
                break;
            case SGX_QL_NO_QUOTE_COLLATERAL_DATA :
                p_log->err("The Quote Library was available, but the quote library could not retrieve the data.\n");
                break;
            case SGX_QL_ERROR_QVL_QVE_MISMATCH:
                p_log->err("Only returned when the quote verification library supports both the untrusted mode of verification and the QvE backed mode of verification. This error indicates that the 2 versions of the verification modes are different. Most caused by using a QvE that does not match the version of the DCAP installed.\n");
                break;
            case SGX_QL_ERROR_UNEXPECTED:
                p_log->err("An unexpected internal error occurred.\n");
                break;
            case SGX_QL_UNKNOWN_MESSAGE_RESPONSE:
                p_log->err("Unexpected error from the attestation infrastructure while retrieving the platform data.\n");
                break;
            case SGX_QL_ERROR_MESSAGE_PARSING_ERROR:
                p_log->err("Generic message parsing error from the attestation infrastructure while retrieving the platform data.\n");
                break;
            case SGX_QL_PLATFORM_UNKNOWN:
                p_log->err("This platform is an unrecognized SGX platform.\n");
                break;
            default:
                p_log->err("undefined error: sgx_qv_verify_quote failed: 0x%04x\n", dcap_ret);
            }
            ret_body["message"] = "Verify quote failed!";
            ret_body["status_code"] = 500;
            return;
        }

        //check verification result
        switch (quote_verification_result)
        {
        case SGX_QL_QV_RESULT_OK:
            p_log->info("App: Verification completed successfully.\n");
            ret_body["message"] = "Verify quote successfully!";
            ret_body["status_code"] = 200;
            break;
        case SGX_QL_QV_RESULT_CONFIG_NEEDED:
        case SGX_QL_QV_RESULT_OUT_OF_DATE:
        case SGX_QL_QV_RESULT_OUT_OF_DATE_CONFIG_NEEDED:
        case SGX_QL_QV_RESULT_SW_HARDENING_NEEDED:
        case SGX_QL_QV_RESULT_CONFIG_AND_SW_HARDENING_NEEDED:
            //p_log->warn("App: Verification completed with Non-terminal result: %x\n", quote_verification_result);
            p_log->info("App: Verify quote successfully in condition! Status code: %x\n", quote_verification_result);
            ret_body["message"] = "Verify quote successfully in condition!";
            ret_body["status_code"] = 200;
            break;
        case SGX_QL_QV_RESULT_INVALID_SIGNATURE:
        case SGX_QL_QV_RESULT_REVOKED:
        case SGX_QL_QV_RESULT_UNSPECIFIED:
        default:
            p_log->err("App: Verification completed with Terminal result: %x\n", quote_verification_result);
            ret_body["message"] = "Verify quote failed!";
            ret_body["status_code"] = 500;
            break;
        }

        // Construct the quote report payload only if the quote verification success
        if (ret_body["status_code"].ToInt() == 200)
        {           
            // Construct the payload data for signing
            // RegisterPayload defined in Crust Mainnet is as follows:
            //      pub struct RegisterPayload<Public, AccountId> {
            //          code: Vec<u8>,
            //          who: AccountId,
            //          pubkey: Vec<u8>,
            //          public: Public
            //      }
            // Need to follow the Substrate Scale Codec specification to encode the signature data
            // 1. [code] is Vector type, encoded by concatening the encodings of its items and 
            //    prefixing with the compactly encoded length of the vector.
            //    code is 32 bytes long, the compact encoding of 32 is 0x80
            // 2. [who] is AccountId trait, encoded by the raw byte stream
            // 3. [pubkey] is Vector type, encoded by concatening the encodings of its items and 
            //    prefixing with the compactly encoded length of the vector.
            //    pubkey is 64 bytes long, the compact encodeing of 64 is 0x01 0x01
            // 4. [public] is Public trait with actual type of sp_runtime::MultiSigner, which is a enum,
            //    so encoded by the u8-index of the respective variant, followed by the encoded value if it is present.
            //    For sp_runtime::MultiSigner with Sr25519 enum value, the u8-index is 0x01, and the encoded value is the 
            //    sr25519 public key byte stream
            std::vector<uint8_t> sig_data;

            const uint8_t *p_account_id = hexstring_to_bytes(account_id.c_str(), account_id.size());
            const uint8_t *p_dcap_pubkey = hexstring_to_bytes(dcap_pubkey.c_str(), dcap_pubkey.size());

            sig_data.insert(sig_data.end(), 0x80);
            sig_data.insert(sig_data.end(), p_mr_enclave, p_mr_enclave + sizeof(sgx_measurement_t));
            sig_data.insert(sig_data.end(), p_account_id, p_account_id + account_id.size()/2);
            sig_data.insert(sig_data.end(), 0x01);
            sig_data.insert(sig_data.end(), 0x01);
            sig_data.insert(sig_data.end(), p_pub_key, p_pub_key + sizeof(sgx_report_data_t));
            sig_data.insert(sig_data.end(), 0x01);
            sig_data.insert(sig_data.end(), p_dcap_pubkey, p_dcap_pubkey + dcap_pubkey.size()/2);

            // Perform the sr25519 signing with the sr25519 key pair
            std::vector<uint8_t> sig(SR25519_SIGNATURE_SIZE, 0);
            sr25519_sign(sig.data(), kp.data() + SR25519_SECRET_SIZE, kp.data(),
                        sig_data.data(), sig_data.size());

            // Construct the report body
            std::string code = hexstring(p_mr_enclave, sizeof(sgx_measurement_t));
            std::string tee_pubkey = hexstring(p_pub_key, sizeof(sgx_report_data_t));

            // Need to prefix with '0x' as the sworker enclave code deal with this prefix
            report_body["payload"]["code"] = "0x" + code;
            report_body["payload"]["who"] = "0x" + account_id;
            report_body["payload"]["pubkey"] = "0x" + tee_pubkey;
            report_body["payload"]["public"]["sr25519"] = "0x" + dcap_pubkey;
            report_body["signature"]["sr25519"] = "0x" + hexstring(sig.data(), sig.size());
        }

    });

    svr.listen(host.c_str(), port);
}
