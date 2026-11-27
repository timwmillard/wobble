return {
    config = {
        dir = 'deps',
    },
    {
        'timwmillard/cbase',
        subdir = '',
        flatten = true,
        files = { 'lib/base.h' },
    },
    {
        'oscar-soirey/Raven',
        flatten = false,
        files = {
            'CMakeLists.txt',
            'LICENSE',
            'NOTICE',
            'cmake/**',
            'include/**',
            'src/**',
            'third_party/**',
            'web/CMakeLists.txt',
            'web/include/**',
            'web/src/**',
        },
    },
    {
        'timwmillard/cbase',
        dev = '..',
        name = 'cbase-tool',
        dir = '.',
        subdir = '',
        flatten = false,
        files = {
            'tool/embedc/embedc.c',
            'tool/embedc/CMakeLists.txt',
        },
    },
}
