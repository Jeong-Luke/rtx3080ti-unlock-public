# UEFI 소스 빌드 — GA102 RTX 3080 Ti port 0.4.0-rtx3080ti

이 묶음은 소스 전용입니다. NVIDIA 펌웨어 payload, vendor archives, prebuilt EFI 및 로컬 prebuilt manifest는 포함하지 않습니다. 빌드 결과는 서명되지 않습니다. `prepare_usb.py`는 `uefi/prebuilt/`의 EFI와 로컬 manifest를 읽으므로 아래에서 직접 생성해야 합니다. 이 문서는 빌드·검사 방법이며 설치·부팅 승인이 아닙니다.

한 대의 정확한 보드(`10de:2208`, subsystem `10de:1535`, 12288 MiB, VBIOS `94.02.71.00.83`)에서 Windows NVIDIA `617.42`로 SS0=`0x88888888`, SS1=`0x6` 보존, FLR 및 Windows 복귀를 완료한 관찰이 있습니다. 이는 단일 보드 결과일 뿐 일반 호환성이나 장애 후 복구 증명이 아닙니다. 로컬에서 새로 빌드한 EFI의 해시가 그 관찰에 사용된 이미지와 같다고 가정하지 마세요.

## 1. 요구 사항

- Linux x86-64
- GCC, binutils (`ld`, `objcopy`, `nm`)
- Python 3.12 이상
- GNU EFI 3.0.15 개발 헤더·라이브러리 (기본 경로 `/usr/include/efi`, `/usr/lib`)
- 전체 테스트: Bash, make, zstd, `systemd-analyze`의 `--root` 검증 기능

검증한 구성은 WSL2 / Ubuntu 24.04, GCC 13.3, GNU EFI 3.0.15, systemd 255입니다. 다른 systemd 버전의 `--root` 동작은 검증하지 않았습니다. 부팅 훅 테스트는 호스트의 `modprobe` 경로가 아닌 임시 이미지 안의 실행 파일 fixture를 검증하며, 커널 모듈을 실행하지 않습니다.

기본 경로가 다르면 `EFI_INCLUDE`, `EFI_LIB`를 지정합니다.

```bash
EFI_INCLUDE=/path/to/efi EFI_LIB=/path/to/lib bash uefi/build.sh
```

## 2. EFI 빌드

```bash
bash uefi/build.sh
```

결과:

```text
build/uefi/GA102-3080TI-PROBE.efi
build/uefi/GA102-3080TI-HANDOFF.efi
build/uefi/GA102-3080TI-UNLOCK.efi
```

`build.sh`는 네트워크를 사용하지 않습니다. `-std=gnu11 -O2 -fpic -ffreestanding -Wall -Wextra -Werror`로 컴파일하고, GNU EFI의 `elf_x86_64_efi.lds`로 링크한 뒤 `.rodata*`와 relocation 섹션을 보존해 PE32+ EFI로 변환합니다. 미해결 ELF 심볼이 있으면 EFI 출력을 만들지 않습니다. 세 파일 모두 서명되지 않았습니다.

조회 EFI는 `-DGU_PROBE_ONLY`로 빌드되어 `execute=1`이 있어도 GPU 설정을 쓰지 않습니다.

`-DGU_HANDOFF_ONLY`로 빌드한 인계 확인 EFI는 GPU를 변경하지 않고 대상의 PCI/GOP 상태를 기록한 뒤 Windows를 시작합니다. 리셋 후 복구나 언락 성공을 검증하는 것은 아닙니다. 기존 준비 도구·prebuilt manifest의 두 이미지 선택은 그대로 유지하며 이 새 이미지를 자동 배치하지 않습니다. 상세 조건은 `uefi/README.md`를 참조합니다.

## 3. 오프라인 검사

EFI 준비·정책·런타임 검사 실행 (C 컴파일러와 GNU EFI 필요):

```bash
python3 -m unittest discover -s tests -p 'test_uefi*.py' -v
```

이 검사는 호스트용 harness를 사용하며 GPU를 읽거나 실행하지 않고 네트워크도 사용하지 않습니다.

전체 오프라인 검사에는 아래 두 pinned 입력을 사전에 로컬에 준비해야 합니다. 이미 검증된 입력이 `audit-inputs/`에 있다면 첫 명령은 생략할 수 있습니다. `fetch_artifacts.py`는 캐시가 비어 있으면 네트워크에서 가져오므로 오프라인 환경에서는 미리 파일을 제공해야 합니다. NVIDIA 바이너리 패키지와 source archive는 이 source-only 복사본에 포함되지 않습니다.

```bash
python3 firmware/fetch_artifacts.py audit-inputs
make prepare
make test
```

`make prepare`는 `audit-inputs/`의 로컬 파일만 읽고 SHA-256을 확인한 뒤 `build/firmware/`와 `build/offline-audit.json`을 만듭니다. 네트워크를 사용하지 않습니다. `make test`도 네트워크 없이 `GPU_UNLOCK_ASSETS=build/firmware`로 Python·C 검사들을 실행합니다. 전체 검사에는 C compiler, Bash, `make`, `zstd`, GNU EFI 개발 파일, `systemd-analyze`의 `--root` 검증 기능이 필요합니다. 하드웨어 ROM 통합 검사는 별도 환경 변수가 설정되지 않으면 실행되지 않습니다. 커널 모듈 빌드는 `make stage`가 KDIR 커널 헤더를 사용합니다.

호스트에 설치하지 않는 패키징:

```bash
make stage
```

`build/stage` 아래에만 씁니다.

```text
build/stage/usr/lib/firmware/rtx3080ti-unlock/610.43.03/*.bin
build/stage/usr/lib/modules/$(uname -r)/extra/rtx3080ti_unlock.ko
build/stage/usr/share/licenses/rtx3080ti-unlock/
```

설치 타깃은 없습니다. 부팅 훅 설치, initramfs 재생성, 모듈 적재, 부팅 항목 등록도 이 저장소의 빌드 단계가 수행하지 않습니다.

## 4. `uefi/prebuilt/` manifest

빌드한 EFI를 준비 도구가 읽을 수 있게 복사하고 SHA-256 manifest를 만듭니다. 저장소 루트에서 실행합니다.

```bash
python3 - <<'PYBUILD'
from pathlib import Path
import hashlib, json, shutil
root = Path.cwd()
prebuilt = root / 'uefi/prebuilt'
prebuilt.mkdir(parents=True, exist_ok=True)
sha = {}
for name in ('GA102-3080TI-PROBE.efi', 'GA102-3080TI-UNLOCK.efi'):
    source = root / 'build/uefi' / name
    if not source.is_file():
        raise SystemExit(f'Build first: {source}')
    target = prebuilt / name
    shutil.copy2(source, target)
    sha[name] = hashlib.sha256(target.read_bytes()).hexdigest()
manifest = {
    'version': '0.4.0-rtx3080ti',
    'target': '10de:2208 subsystem 10de:1535 12288 MiB',
    'architecture': 'x86_64 UEFI',
    'signed': False,
    'hardware_tested': False,
    'sha256': sha,
}
(prebuilt / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
PYBUILD
```

이 명령은 `PROBE`와 `UNLOCK`만 복사합니다. `HANDOFF` EFI는 별도 산출물이며 `prepare_usb.py`가 처리하거나 배치하지 않습니다. manifest의 `hardware_tested=false`는 방금 로컬에서 만든 각 해시가 관찰에 사용된 EFI와 동일하다고 확인하지 않았다는 의미입니다. `uefi/prebuilt/`는 `.gitignore` 대상이며 source-only 배포에는 포함되지 않습니다.

준비 도구의 실행 모드 출력 `hardware validation pending`도 새 준비본의 실물 검증을 대신하지 않는다는 경고로 유지했습니다. 과거 한 장비의 관측을 새 파일이나 새 PC의 통과 판정으로 자동 승계하지 않습니다.

## 5. 펌웨어 자산

핀된 610.43.03 자산의 파일 이름과 내용은 upstream 그대로 유지합니다.

```text
booter_load_ga102.bin
gsp_ga10x.bin
gsp_boot_ga102.bin
gsp_boot_desc_ga102.bin
v67-ga102-610.43.03.bin
NVIDIA-LICENSE
```

`firmware/artifacts-610.43.03.json`의 출처·SHA-256·생성 규칙을 유지합니다. `make prepare`는 로컬 입력만 사용합니다. `prepare_usb.py --execute`는 캐시에 없는 자산을 pinned URL에서 내려받을 수 있으므로, 완전한 오프라인 준비는 미리 채운 `--cache` 디렉터리로 수행합니다.

`prepare_usb.py`는 자산과 EFI 해시를 검증한 뒤 새 일반 폴더에만 staging 합니다. 출력 폴더는 `ga102-3080ti/`이며, 기본은 SS1 보존입니다. 이 도구는 드라이브 포맷, BCD/부팅 항목 등록, 자동 시작 설정을 하지 않습니다.

## 6. 검증 범위와 남은 한계

관찰된 실기 결과는 한 보드에서 SS0=`0x88888888`, SS1=`0x6` 보존, FLR 및 Windows 복귀가 완료된 것입니다. 이는 다른 보드와 시스템의 호환성, 실패 후 복구, 반복·장시간 안정성을 증명하지 않습니다. 이 빌드 문서는 설치 승인이 아닙니다. 다음 항목은 별도로 검토·검증해야 합니다.

- Secure Boot·서명 정책, DMA/IOMMU, 실패 시 정지·로그·watchdog 정책 검토
- GOP/디스플레이 전환, 화면 유지 및 복구 경로 검증
- UEFI entry/policy, runtime, offline HAL, 커널 모듈의 통합 검사
- 별도 부팅 승인

현재 이 EFI를 부팅하거나 설치하지 않습니다.

## 7. 라이선스

저장소의 `GPL-2.0-only`, `uefi/GNU-EFI-NOTICE.txt`, `firmware/NOTICE.md`, NVIDIA 라이선스 고지를 보존합니다. source-only 복사본은 NVIDIA firmware binary를 포함하지 않으며, 이 문서는 펌웨어나 vendor archive의 재배포 승인을 주장하지 않습니다.