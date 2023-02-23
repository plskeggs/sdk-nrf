#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <modem/modem_jwt.h>
#include <date_time.h>
#include <net/nrf_cloud.h>
#include <psa/crypto.h>
#include <psa/crypto_extra.h>

#ifdef CONFIG_BUILD_WITH_TFM
#include <tfm_ns_interface.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(nrf_cloud_coap_client, CONFIG_NRF_CLOUD_COAP_CLIENT_LOG_LEVEL);

#define JWT_DURATION_S	(60*5)
#define JWT_BUF_SZ	900
/* Buffer used for JSON Web Tokens (JWTs) */
//static char jwt[JWT_BUF_SZ];
static psa_key_handle_t keypair_handle;
static psa_key_handle_t pub_key_handle;

int jwt_init(void)
{
	cJSON_Init();

	psa_status_t status;

	/* Initialize PSA Crypto */
	status = psa_crypto_init();
	if (status != PSA_SUCCESS) {
		return -1;
	}
	return 0;
}

int import_ecdsa_pub_key(uint8_t *m_pub_key, size_t len_pub_key)
{
	/* Configure the key attributes */
	psa_key_attributes_t key_attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_status_t status;

	/* Configure the key attributes */
	psa_set_key_usage_flags(&key_attributes, PSA_KEY_USAGE_VERIFY_HASH);
	psa_set_key_lifetime(&key_attributes, PSA_KEY_LIFETIME_VOLATILE);
	psa_set_key_algorithm(&key_attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_type(&key_attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&key_attributes, 256);

	status = psa_import_key(&key_attributes, m_pub_key, sizeof(m_pub_key), &pub_key_handle);
	if (status != PSA_SUCCESS) {
		LOG_INF("psa_import_key failed! (Error: %d)", status);
		return APP_ERROR;
	}

	/* After the key handle is acquired the attributes are not needed */
	psa_reset_key_attributes(&key_attributes);

	return APP_SUCCESS;
}

int sign_message(void)
{
	uint32_t output_len;
	psa_status_t status;

	LOG_INF("Signing a message using ECDSA...");

	/* Compute the SHA256 hash*/
	status = psa_hash_compute(PSA_ALG_SHA_256,
				  m_plain_text,
				  sizeof(m_plain_text),
				  m_hash,
				  sizeof(m_hash),
				  &output_len);
	if (status != PSA_SUCCESS) {
		LOG_INF("psa_hash_compute failed! (Error: %d)", status);
		return APP_ERROR;
	}

	/* Sign the hash */
	status = psa_sign_hash(keypair_handle,
			       PSA_ALG_ECDSA(PSA_ALG_SHA_256),
			       m_hash,
			       sizeof(m_hash),
			       m_signature,
			       sizeof(m_signature),
			       &output_len);
	if (status != PSA_SUCCESS) {
		LOG_INF("psa_sign_hash failed! (Error: %d)", status);
		return APP_ERROR;
	}

	LOG_INF("Signing the message successful!");
	PRINT_HEX("Plaintext", m_plain_text, sizeof(m_plain_text));
	PRINT_HEX("SHA256 hash", m_hash, sizeof(m_hash));
	PRINT_HEX("Signature", m_signature, sizeof(m_signature));

	return APP_SUCCESS;
}

int jwt_generate(uint32_t time_valid_s, char *const jwt_buf, size_t jwt_buf_sz)
{
	if (!jwt_buf || !jwt_buf_sz) {
		return -EINVAL;
	}

	#define GET_TIME_CMD "AT%%CCLK?"
	int err;
	char buf[NRF_CLOUD_CLIENT_ID_MAX_LEN + 1];
	struct jwt_data jwt = {
		.audience = NULL,
		.sec_tag = CONFIG_COAP_SECTAG,
		.key = JWT_KEY_TYPE_CLIENT_PRIV,
		.alg = JWT_ALG_TYPE_ES256,
		.jwt_buf = jwt_buf,
		.jwt_sz = jwt_buf_sz
	};

	/* Check if modem time is valid */
	err = nrf_modem_at_cmd(buf, sizeof(buf), GET_TIME_CMD);
	if (err != 0) {
		LOG_ERR("Modem does not have valid date/time, JWT not generated");
		return -ETIME;
	}

	if (time_valid_s > NRF_CLOUD_JWT_VALID_TIME_S_MAX) {
		jwt.exp_delta_s = NRF_CLOUD_JWT_VALID_TIME_S_MAX;
	} else if (time_valid_s == 0) {
		jwt.exp_delta_s = NRF_CLOUD_JWT_VALID_TIME_S_DEF;
	} else {
		jwt.exp_delta_s = time_valid_s;
	}

	if (IS_ENABLED(CONFIG_NRF_CLOUD_CLIENT_ID_SRC_INTERNAL_UUID)) {
		/* The UUID is present in the iss claim, so there is no need
		 * to also include it in the sub claim.
		 */
		jwt.subject = NULL;
	} else {
		err = nrf_cloud_client_id_get(buf, sizeof(buf));
		if (err) {
			LOG_ERR("Failed to obtain client id, error: %d", err);
			return err;
		}
		jwt.subject = buf;
	}

	cJSON *header_obj = cJSON_CreateObject();

	err = cJSON_AddStringToObjectCS(header_obj, "alg", "HS256");
	err = cJSON_AddStringToObjectCS(header_obj, "typ", "JWT");
	char *header = cJSON_PrintUnformatted(header_obj);

	cJSON_Delete(header_obj);

	int64_t now;
	cJSON *payload_obj = cJSON_CreateObject();

	date_time_now(&now);
	now /= 1000;

	if (jwt.subject) {
		err = cJSON_AddStringToObjectCS(payload_obj, "sub", jwt.subject);
	}
	err = cJSON_AddNumberToObjectCS(payload_obj, "iat", now);
	err = cJSON_AddNumberToObjectCS(payload_obj, "exp", now + jwt.exp_delta_s);
	/* iss field contains "nRF9160.<device uuid>" */

	char *payload = cJSON_PrintUnformatted(payload_obj);

	cJSON_Delete(payload_obj);

	size_t hlen;
	size_t plen;

	base64_encode(NULL, 0, &hlen, header, strlen(header));
	base64_encode(NULL, 0, &plen, payload, strlen(payload));

	char *msg = k_malloc(hlen + 1 + plen + 1);

	base64_encode(msg, hlen, &hlen, header, strlen(header));
	msg[hlen] = '.';
	base64_encode(&msg[hlen + 1], plen, &plen, payload, strlen(payload));
	msg[hlen + 1 + plen] = '\0';


	
	/* replace + with - and / with _ */

/*
JWT: eyJ0eXAiOiJKV1QiLCJhbGciOiJFUzI1NiIsImtpZCI6IjA3MDM3NWJkMjQxMTNlOGI4MzM4ZWRiZjUxZDg1NDliMjhjYTBlZjIwMmIxNjA0NGVlOGZjY2EyMjE1NDBiZDYifSAg.eyJpc3MiOiJuUkY5MTYwLjUwNGU1MzUzLTM4MzEtNDVjOC04MDBhLTI3MTlkOGJhMzlmNSIsImp0aSI6Im5SRjkxNjAuZjZlM2JkN2ItN2RmMy00Yjg4LWI0M2EtYTdhOTc2NmEwNGFkLjJmNzM1M2UwYTdkMzMzNWE0N2NmYjFhZDdlM2U5YWNiIiwiaWF0IjoxNjc2OTMwMzU4LCJleHAiOjE2NzY5MzA2NTgsInN1YiI6Im5yZi0zNTI2NTYxMDYxMDgxNDgifSAg.uv7rse7I4-peOgYU2twZc9HpItQU4K3JBYc7vEXPK7S7oLTvfDBoP_63T1J5EIquXLmCTmSTXLPEVcMkJm24FQ
 
header: {
  "typ": "JWT",
  "alg": "ES256",
  "kid": "070375bd24113e8b8338edbf51d8549b28ca0ef202b16044ee8fcca221540bd6"
}

payload: {
  "iss": "nRF9160.504e5353-3831-45c8-800a-2719d8ba39f5",
  "jti": "nRF9160.f6e3bd7b-7df3-4b88-b43a-a7a9766a04ad.2f7353e0a7d3335a47cfb1ad7e3e9acb",
  "iat": 1676930358,
  "exp": 1676930658,
  "sub": "nrf-352656106108148"
}

verify signature:
ECDSASHA256(
  base64UrlEncode(header) + "." +
  base64UrlEncode(payload),
  Public Key in SPKI, PKCS #1, X.509 Certificate, or JWK string format.
  ,
  Private Key in PKCS #8, PKCS #1, or JWK string format. The key never leaves your browser.
)

NOTE:
- omit iss if using sub
- exp = expiration time after which should not be used
- iat = issued at -- time it was issued
- jti = jwt id; must be unique to prevent replay
- kid = key id to perform key lookup
*/

	if (err) {
		LOG_ERR("Failed to generate JWT, error: %d", err);
	}

	return err;
}

