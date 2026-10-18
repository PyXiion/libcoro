#define CATCH_CONFIG_MAIN
#include "catch_amalgamated.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <signal.h>

#ifdef LIBCORO_FEATURE_NETWORKING
/**
 * This structure invokes a constructor to setup some global test settings that are needed prior
 * to executing the tests.
 */
struct test_setup_networking
{
    test_setup_networking()
    {
    #ifdef SIGPIPE
        // Ignore SIGPIPE, the library should be handling these gracefully.
        signal(SIGPIPE, SIG_IGN);
    #endif

    #ifdef LIBCORO_FEATURE_TLS
        // For SSL/TLS tests create a localhost cert.pem and key.pem, tests expected these files
        // to be generated into the same directory that the tests are running in.
        if (std::system(
                "openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365 -subj \"/CN=localhost\" -nodes") !=
            0)
        {
            std::cerr << "Failed to generate the TLS test certificate and private key with openssl.\n";
            std::exit(EXIT_FAILURE);
        }
    #endif
    }

    ~test_setup_networking()
    {
    #ifdef LIBCORO_FEATURE_TLS
        // Cleanup the temporary key.pem and cert.pem files.
        std::filesystem::remove("key.pem");
        std::filesystem::remove("cert.pem");
    #endif
    }
};

static test_setup_networking g_test_setup_networking{};
#endif
