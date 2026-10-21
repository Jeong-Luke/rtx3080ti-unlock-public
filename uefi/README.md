# GA102 RTX 3080 Ti UEFI 실험 소스 — 0.4.0-rtx3080ti

> **소스 전용, 부팅 미승인.** 한 보드에서만 `10de:2208`, subsystem `10de:1535`, 12288 MiB, VBIOS `94.02.71.00.83`, Windows NVIDIA `617.42` 조합으로 SS0=`0x88888888`, SS1=`0x6` 보존, FLR 및 Windows 복귀가 완료된 관찰이 있습니다. 이는 일반 호환성이나 복구 증명이 아닙니다. 이 EFI를 지금 부팅하거나 설치하도록 안내하지 않습니다.

## 1. 대상과 산출물

| 항목 | 값 |
|---|---|
| PCI vendor/device | `10de:2208` |
| Subsystem vendor/device | `10de:1535` |
| VRAM | 정확히 `12288` MiB |
| 조회 EFI | `GA102-3080TI-PROBE.efi` |
| GPU 무변경 Windows 인계 확인 EFI | `GA102-3080TI-HANDOFF.efi` |
| 실행 EFI | `GA102-3080TI-UNLOCK.efi` |
| 설정·자산 폴더 | `ga102-3080ti/` |
| 로그 | `ga102-3080ti-log.txt` |
| 버전 | `0.4.0-rtx3080ti` |

`10de:2208`이어도 subsystem이 `10de:1535`가 아니거나 12288 MiB가 아니면 거절합니다. 보드가 정확히 한 장으로 확인되지 않으면 실행 설정을 만들지 않습니다.

## 2. 현재 검증 상태

- 한 대의 정확한 대상 보드에서 VBIOS `94.02.71.00.83` 및 Windows NVIDIA 드라이버 `617.42`로 실행해 SS0=`0x88888888`, SS1=`0x6` 보존, FLR 및 Windows 복귀를 완료했습니다.
- 이 결과는 해당 한 대에 대한 단일 관찰입니다. 다른 보드·시스템의 호환성, 장애 후 복구, 반복·장시간 안정성을 증명하지 않습니다.
- WSL2 / Ubuntu 24.04의 GCC 13.3 / GNU EFI 3.0.15에서 세 EFI를 빌드했습니다. 산출물은 `build/uefi/`에 있으며 소스 배포에는 포함하지 않습니다.
- 현재 부팅 승인이 없습니다. 준비 폴더를 만드는 것과 실제 부팅은 별개이며, 아래 보안·디스플레이·통합 검토가 남아 있습니다.

## 3. EFI 설계

```text
실행 파티션 루트
  → ga102-3080ti/config.txt 읽기
  → PCI inventory 또는 execute=1 거래
  → 대상 ID·subsystem·용량, Windows Boot Manager 검사
  → (실행 모드) 자산 SHA-256, FWSEC/V67, FLR, readback
  → 검증 성공 시에만 Windows Boot Manager로 인계
```

- **조회 EFI**는 `execute=1`이 있어도 GPU 설정을 쓰지 않고 PCI 정보와 로그만 남깁니다.
- **인계 확인 EFI**는 `GU_HANDOFF_ONLY` 전용 빌드입니다. `execute=1`도 무시하고 GPU 리셋·언락 없이 정확한 대상 PCI 장치의 GOP 메타데이터와 PCI 설정을 기록한 뒤 Windows Boot Manager를 로드·시작합니다. `target`, `windows=1`과 정상적인 대상 선형 GOP가 필요하며 `windows_partition`을 지정해 Windows 파티션을 고정할 수 있습니다. 앱 자체의 GPU 쓰기·MMIO·펌웨어 요청은 없지만 Windows가 시작되면 정상 드라이버 초기화는 일어납니다. 로그 파일 쓰기와 실제 부팅은 수행하므로 별도 실물 승인이 필요합니다.
- **실행 EFI**는 `execute=1`일 때만 거래를 시도합니다. 기본 설정은 `execute=0`입니다.
- 설정 키는 `target`, `board_vendor`, `board_device`, `execute`, `ss1_full`, `windows`, 선택적 `windows_partition`입니다.
- 기본은 **SS1 보존**(`ss1_full=0`)입니다. `ss1_full=1`은 별도 실험이며 기본값이 아닙니다.
- 실행 전 검사 실패는 펌웨어로 반환할 수 있지만, 실행 이후 실패·DMA 정리 불확실·`windows=0` 진단 모드는 Windows로 넘어가지 않고 정지합니다.
- 로그는 실행 파티션 루트의 `ga102-3080ti-log.txt`에 기록합니다.
- 이 앱은 BCD, BootOrder, Secure Boot 설정을 변경하지 않습니다.

인계 확인 EFI는 **언락 후 화면 복구 시험이 아닙니다.** 먼저 리셋 없는 Windows 인계와 현재 GOP/PCI 기준 상태를 확인하기 위한 단계입니다. GOP 구조체가 정상이어도 실제 화면 출력을 입증하지 않으며, Windows 부트 매니저가 반환한 것을 부팅 성공으로 판정하지 않습니다. GOP `SetMode`, 드라이버 재연결 또는 추측한 GPU 레지스터 복원은 하지 않습니다. 현재 준비 도구의 `probe`/`unlock` 모드는 HANDOFF EFI를 자동 선택하지 않으므로 별도 준비와 확인이 필요합니다.

설정과 자산은 실행 EFI가 시작된 파티션의 루트에서 `ga102-3080ti/` 아래로 읽습니다. EFI 파일만 다른 파티션으로 옮기면 설정·펌웨어를 찾지 못합니다.

## 4. 준비 도구

`prepare_usb.py`는 **새 일반 폴더**에 검토용 파일을 staging 합니다. 기존 경로, USB 루트, Windows EFI 파티션을 출력으로 지정하면 거절합니다.

```powershell
python .\uefi\prepare_usb.py --output .\NEW_PROBE_FOLDER
python .\uefi\prepare_usb.py --output .\NEW_EXEC_FOLDER --mode unlock --execute
```

- 조회 모드는 펌웨어를 내려받지 않습니다.
- `--execute`는 `--mode unlock`에서만 허용되는 명시적 opt-in입니다.
- 실행 모드는 `nvidia-smi`로 정확한 보드 한 장을 확인합니다. 감지에 실패하면 실행 폴더를 만들지 않습니다.
- EFI는 `uefi/prebuilt/manifest.json`의 SHA-256과 일치해야 합니다.
- 펌웨어는 pinned manifest의 SHA-256을 검증한 뒤에만 포함합니다.
- 드라이브 포맷, BCD/부팅 항목 등록, 부팅 순서 변경, 자동 시작 설치는 하지 않습니다.

준비 폴더의 구조는 다음과 같습니다. 복사·부팅 절차가 아니며, 현재 승인된 설치 단계도 아닙니다.

```text
EFI/BOOT/BOOTX64.EFI
ga102-3080ti/config.txt
ga102-3080ti/firmware/booter_load_ga102.bin
ga102-3080ti/firmware/gsp_ga10x.bin
ga102-3080ti/firmware/gsp_boot_ga102.bin
ga102-3080ti/firmware/gsp_boot_desc_ga102.bin
ga102-3080ti/firmware/v67-ga102-610.43.03.bin
ga102-3080ti/firmware/NVIDIA-LICENSE
README.md
```

`config.txt`의 대상 줄은 감지된 BDF와 subsystem에서 만들며, 감지되지 않은 보드 값을 예시로 채우지 않습니다.

## 5. 빌드

Linux x86-64, GCC/binutils, GNU EFI 3.0.15 기준입니다. 네트워크를 사용하지 않습니다.

```bash
bash uefi/build.sh
python3 -m unittest discover -s tests -p 'test_uefi*.py' -v
```

결과는 `build/uefi/GA102-3080TI-PROBE.efi`, `build/uefi/GA102-3080TI-HANDOFF.efi`, `build/uefi/GA102-3080TI-UNLOCK.efi`입니다. `-Wall -Wextra -Werror`로 컴파일하고, 미해결 ELF 심볼이 있으면 EFI 출력을 만들지 않습니다. `.rodata*`와 relocation 섹션을 보존합니다. 빌드된 EFI는 서명되지 않았습니다.

prebuilt manifest를 만드는 절차와 오프라인 검사는 [빌드 문서](../docs/BUILD.md)에 있습니다. 소스 검토나 Python 테스트만으로 EFI 빌드·C 런타임 검증을 대신할 수 없습니다.

## 6. 부팅 전에 남은 게이트

**보안**
- 서명되지 않은 EFI의 Secure Boot 정책·배포 방식 미정.
- PCI 설정 쓰기, BME/WPR2, DMA CommonBuffer map/unmap, IOMMU 환경 검증 필요.
- 실패 시 정지·로그 저장·watchdog·재시도 금지 정책의 실기 검증 필요.
- UEFI entry/policy 상수와 이 문서의 대상 ID 일치에 대한 독립 검토 필요.

**디스플레이**
- GOP 소유권, 기본 디스플레이 전환, 화면 blank/복구, 다중 모니터 동작 미검증.
- 초기화 실패 시 사용자가 복구할 표시 경로가 보장되지 않음.

**ROM/펌웨어**
- 위 단일 실행에서의 SS0·SS1 결과, FLR 및 Windows 복귀를 넘어서는 일반적인 BootROM/fuse 수락·복구 증명은 없습니다.
- 독립된 재시험, 반복·장시간 안정성 및 실패 후 복구는 검증되지 않았습니다.

**통합**
- 공유 코어·UEFI runtime·offline HAL·패키징 사이의 통합 검사와 C 테스트가 필요합니다. Windows 적용에 Linux 모듈 설치는 필요하지 않습니다.
- 모든 게이트와 별도 부팅 승인 전에는 이 EFI를 부팅하거나 설치하지 않습니다.

## 7. 파일과 라이선스

- EFI 소스는 `uefi/`, 커널 코어는 `kernel/gpu_unlock/`에 있습니다.
- 부팅 훅은 `boot/rtx3080ti-order.install`, `boot/zz-rtx3080ti-unlock.conf`입니다.
- initramfs 판독은 `tools/inspect_initramfs.py`가 `rtx3080ti-unlock.service`와 `rtx3080ti_unlock`을 확인합니다.
- 펌웨어 자산 이름과 내용은 upstream pinned 610.43.03 그대로입니다.
- `GPL-2.0-only`, `GNU-EFI-NOTICE.txt`, `firmware/NOTICE.md`, NVIDIA 라이선스 고지를 보존합니다.

---

이 문서는 소스 포트의 현재 상태를 설명하며, 설치·부팅 절차나 3080 Ti 동작 보증이 아닙니다.