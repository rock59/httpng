dofile("examples/load_ssl_source.lua")

return {
    { certificate_file = foo_cert_path, certificate_key_file = foo_key_path },
    { certificate_file = bar_cert_path, certificate_key_file = bar_key_path },
}