"""Shared configuration for Android diagnostic build tools."""
from pathlib import Path


def configured_ndk_version():
    properties = Path(__file__).resolve().parents[1] / 'gradle.properties'
    for line in properties.read_text(encoding='utf-8').splitlines():
        key, separator, value = line.partition('=')
        if separator and key.strip() == 'vulkanFixNdkVersion' and value.strip():
            return value.strip()
    raise RuntimeError(f'vulkanFixNdkVersion is missing from {properties}')
