# vim:set ft= ts=4 sw=4 et fdm=marker:
use lib 'lib';
use Test::Nginx::Socket::Lua;

#worker_connections(1014);
#master_on();
#workers(2);
#log_level('warn');

repeat_each(2);

plan tests => repeat_each() * (blocks() * 3);

#no_diff();
#no_long_string();
run_tests();

__DATA__

=== TEST 1: nginx configure_args
--- config
    location /configure_args {
        content_by_lua '
            ngx.say(ngx.config.ngx_configure_args)
        ';
    }
--- request
GET /configure_args
--- response_body_like chop
^\s*\-\-[^-]+
--- no_error_log
[error]

