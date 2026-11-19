# Pure ASCII helper with explicit FileShare.ReadWrite stream copy (C-08 fix)
# T4 status: NOT VERIFIED - elevated real-machine retest still required (R7).
# This script only defines a function; it performs no deploy, sign or reboot.
function Copy-RbFileWithReadWriteShare {
    param(
        [Parameter(Mandatory=$true)][string]$SourcePath,
        [Parameter(Mandatory=$true)][string]$DestinationPath
    )
    $srcStream = [System.IO.File]::Open(
        $SourcePath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite
    )
    try {
        $dstStream = [System.IO.File]::Open(
            $DestinationPath,
            [System.IO.FileMode]::Create,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None
        )
        try {
            $srcStream.CopyTo($dstStream)
            $dstStream.Flush()
        }
        finally {
            $dstStream.Dispose()
        }
    }
    finally {
        $srcStream.Dispose()
    }
}
