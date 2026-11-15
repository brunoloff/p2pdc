# Linux TURN-over-TLS tests

The Linux backend requires the libnice and libdatachannel patches in
`patches/`. The libnice TLS socket verifies the peer certificate against
OpenSSL's default trust paths and checks the configured DNS name or IP address.
OpenSSL's normal `SSL_CERT_FILE` and `SSL_CERT_DIR` settings apply. Native TLS
configuration is rejected if the running libnice lacks the patch.

The controlled tests use disposable certificates, credentials, a TURN server,
and an isolated network namespace. They do not modify system certificate stores
or contact public infrastructure. They require `turnserver`, `openssl`, `ip`,
`unshare`, Python, `pkg-config`, and C/C++ compilers.

After building the dependencies and the library as in the README, build the
optional test targets and probes. Set `P2PDC_NICE_SOURCE` and
`P2PDC_NICE_PREFIX` if the patched libnice is not in the README's locations.

```sh
nice -n 15 ionice -c 3 cmake --build build-native --parallel 1 \
  --target p2pdc_native_transport_tests p2pdc_native_turn_tests
nice -n 15 ionice -c 3 bash tests/build_native_tls_probes.sh
nice -n 15 ionice -c 3 bash tests/run_native_tls_feature_check.sh \
  "$PWD/build-native/p2pdc_native_transport_tests"
nice -n 15 ionice -c 3 bash tests/run_native_turn_tls.sh unit
export P2PDC_TLS_TEST_BINARY="$PWD/build-native/p2pdc_native_turn_tests"
for mode in trusted trusted-ip fallback untrusted hostname wrong-dns expired plaintext alternate unreachable; do
  nice -n 15 ionice -c 3 bash tests/run_native_turn_tls.sh "$mode"
done
```

The fallback case also needs `nft`; it checks that direct packets were blocked
and both public peers reported TURN use. The feature check needs an unpatched system libnice. Set
`P2PDC_TEST_UNPATCHED_NICE_DIR` to its library directory if pkg-config resolves
the patched library instead. The `unit` case exercises fragmented TLS records,
partial data followed by close or EOF, and atomic queue admission. The network
cases verify relay use and message exchange for trusted certificates, and lack
of readiness for each negative control. The plaintext control observes a TLS
ClientHello. The alternate-server case also checks that the failed socket
creation branch was reached. These checks do not establish performance under
load, all network conditions, or relay-only privacy.
