# The pinned XIAO load helper leaves its RRAM buffer uncommitted before reset.
proc nrf54l-load-committed {file} {
    set status [catch {
        nrf54l-load $file
        mww 0x5004b008 1
        set ready 0
        for {set attempt 0} {$attempt < 1000} {incr attempt} {
            mem2array ready_value 32 0x5004b400 1
            if {$ready_value(0) == 1} {
                set ready 1
                break
            }
            sleep 1
        }
        if {!$ready} {error "RRAMC READY timeout after write-buffer commit"}
    } detail]
    mww 0x5004b500 0
    if {$status != 0} {error $detail}
    echo "NRF_RRAM_COMMIT: PASS"
}
