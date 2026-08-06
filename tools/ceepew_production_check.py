#!/usr/bin/env python3
import os
import sys

def main():
    print("=== CEE-PEW Production Configuration Checker ===")
    
    project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    sdkconfig_path = os.path.join(project_root, "sdkconfig")
    
    if not os.path.exists(sdkconfig_path):
        print(f"Error: sdkconfig not found at {sdkconfig_path}")
        print("Please configure the project first (e.g. run idf.py build).")
        sys.exit(1)
        
    # Read active config
    config = {}
    with open(sdkconfig_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line:
                key, val = line.split("=", 1)
                config[key.strip()] = val.strip().strip('"')

    errors = []
    
    # 1. Check development mode
    dev_mode = config.get("CONFIG_CEEPEW_DEVELOPMENT_MODE")
    if dev_mode == "y":
        errors.append("CONFIG_CEEPEW_DEVELOPMENT_MODE must be disabled (currently enabled).")
    else:
        print("[PASS] CONFIG_CEEPEW_DEVELOPMENT_MODE is disabled")
        
    # 2. Check OLED graphics test mode
    graphics_test = config.get("CONFIG_CEEPEW_OLED_GRAPHICS_TEST")
    if graphics_test == "y":
        errors.append("CONFIG_CEEPEW_OLED_GRAPHICS_TEST must be disabled (currently enabled).")
    else:
        print("[PASS] CONFIG_CEEPEW_OLED_GRAPHICS_TEST is disabled")
        
    # 3. eFuse HMAC key binding was intentionally dropped by design decision.
    #    No longer a production requirement; keep as informational only.
    efuse_hmac = config.get("CONFIG_CEEPEW_EFUSE_HMAC_KEY")
    if efuse_hmac == "y":
        print("[INFO] CONFIG_CEEPEW_EFUSE_HMAC_KEY is enabled (device-bound salt active)")
    else:
        print("[INFO] CONFIG_CEEPEW_EFUSE_HMAC_KEY is disabled (design decision — eFuse binding dropped)")
        
    # 4. Secure Boot v2 and Flash Encryption were intentionally dropped by
    #    design decision (they burn one-way eFuses). Informational only.
    sb_enabled = config.get("CONFIG_SECURE_BOOT_V2_ENABLED")
    if sb_enabled == "y":
        print("[WARN] CONFIG_SECURE_BOOT_V2_ENABLED is enabled — this burns one-way eFuses on first boot (design decision removed this for dev/reusable firmware).")
    else:
        print("[INFO] CONFIG_SECURE_BOOT_V2_ENABLED is disabled (design decision — no eFuse burn)")

    fe_enabled = config.get("CONFIG_SECURE_FLASH_ENCRYPTION_ENABLED")
    if fe_enabled == "y":
        print("[WARN] CONFIG_SECURE_FLASH_ENCRYPTION_ENABLED is enabled — this burns one-way eFuses on first boot (design decision removed this for dev/reusable firmware).")
    else:
        print("[INFO] CONFIG_SECURE_FLASH_ENCRYPTION_ENABLED is disabled (design decision — no eFuse burn)")

    print("\n=== SUMMARY ===")
    if errors:
        print(f"FAILED: {len(errors)} production checks failed:")
        for err in errors:
            print(f"  - {err}")
        sys.exit(2)
    else:
        print("ALL CHECKS PASSED: Ready for production deployment!")
        sys.exit(0)

if __name__ == "__main__":
    main()
