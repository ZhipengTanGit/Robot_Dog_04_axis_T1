$ErrorActionPreference='Stop'
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    & gcc -std=c99 -Wall -Wextra -Werror -pedantic -Itests/uart3_stubs BSP/src/BSP_UART_3.c tests/test_uart3_async.c -o Output/Tests/uart3-async.exe
    if($LASTEXITCODE){throw 'UART3 build failed'}
    & .\Output\Tests\uart3-async.exe
    if($LASTEXITCODE){throw 'UART3 tests failed'}
} finally {Pop-Location}
