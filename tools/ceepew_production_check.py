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
        
    # 3. Check eFuse HMAC key
    efuse_hmac = config.get("CONFIG_CEEPEW_EFUSE_HMAC_KEY")
    if efuse_hmac != "y":
        errors.append("CONFIG_CEEPEW_EFUSE_HMAC_KEY must be enabled (currently disabled or unset).")
    else:
        print("[PASS] CONFIG_CEEPEW_EFUSE_HMAC_KEY is enabled")
        
    # 4. Check Secure Boot v2 (Release mode)
    sb_enabled = config.get("CONFIG_SECURE_BOOT_V2_ENABLED")
    if sb_enabled != "y":
        errors.append("CONFIG_SECURE_BOOT_V2_ENABLED must be enabled (currently disabled or unset).")
    else:
        print("[PASS] CONFIG_SECURE_BOOT_V2_ENABLED is enabled")
        
        sb_key_path = config.get("CONFIG_SECURE_BOOT_SIGNING_KEY")
        if not sb_key_path:
            errors.append("CONFIG_SECURE_BOOT_SIGNING_KEY is not set.")
        else:
            # Check absolute or relative path
            full_key_path = os.path.join(project_root, sb_key_path)
            if not os.path.exists(full_key_path):
                errors.append(f"Secure Boot signing key file not found at: {sb_key_path}")
            else:
                print(f"[PASS] Secure Boot signing key exists at: {sb_key_path}")
                
    # 5. Check Flash Encryption (Release mode)
    fe_enabled = config.get("CONFIG_SECURE_FLASH_ENCRYPTION_ENABLED")
    if fe_enabled != "y":
        errors.append("CONFIG_SECURE_FLASH_ENCRYPTION_ENABLED must be enabled (currently disabled or unset).")
    else:
        print("[PASS] CONFIG_SECURE_FLASH_ENCRYPTION_ENABLED is enabled")
        
    fe_mode_release = config.get("CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE")
    if fe_mode_release != "y":
        errors.append("CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE must be enabled (currently disabled or unset).")
    else:
        print("[PASS] CONFIG_SECURE_FLASH_ENCRYPTION_MODE is set to RELEASE")

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
