# Swift 씨앗에서 시작하는 부트스트랩

[hmje-swift-seed](https://github.com/Semtle-Inc/hmje-swift-seed)는 Swift로 작성한 초기 컴파일러에서 HMJE 자체 호스팅 컴파일러로 이어지는 재현 절차를 제공합니다. 컴파일러 소스와 실행 절차, 입력 해시, 단계별 검증 보고서는 해당 저장소의 [README](https://github.com/Semtle-Inc/hmje-swift-seed#readme)와 [부트스트랩 단계](https://github.com/Semtle-Inc/hmje-swift-seed/blob/main/docs/bootstrap.md)에 있습니다.

## 소스 빌드와 재현 검증

이 저장소는 공개 소스의 빌드와 설치를 담당합니다. `hmje-swift-seed`는 Beta 1 빌드 `2026100804`의 네 도구(`컴파일러`, `hmir을llvm으로`, `제품조정기`, `프로젝트조정기`)를 Swift 씨앗에서 구축하고, 세대 간 고정점과 배포 파일과의 일치를 검사합니다.

재현 검증에 제공하는 Release 파일은 비교 대상입니다. 초기 컴파일러로 실행하지 않습니다. 코드 서명과 UUID 등을 정규화한 비교를 포함하므로, 서명된 배포 파일 전체의 바이트 재현과 구분합니다.

## 요구 환경

Swift 재현 절차의 확인 환경은 macOS ARM64입니다. 초기 도구의 배포 대상 때문에 macOS 26 이상을 실행하는 Apple Silicon Mac이 필요합니다. 전체 절차는 Swift 6.4, Apple Clang 21, macOS SDK 27.0에서 확인했습니다. 최종 Beta 1 도구의 배포 대상은 macOS 14.0입니다.

호스트 도구와 비교 파일의 정확한 요구 사항은 `hmje-swift-seed`의 요구 사항 및 해시 목록을 따릅니다. Windows·Linux와 다른 macOS 아키텍처의 소스 빌드는 [Release 툴체인 경로](building.md)를 사용합니다.

## 실행

[HMJE 소스](https://github.com/Semtle-Inc/hmje)와 [Swift 씨앗](https://github.com/Semtle-Inc/hmje-swift-seed)을 같은 상위 디렉터리에 체크아웃한 예입니다. `/path/to/beta1/libexec/bin`은 Beta 1 macOS ARM64 패키지에 포함된 네 도구의 실제 경로로 바꿉니다. 검증 출력 폴더는 새 경로여야 합니다.

```sh
cd ../hmje-swift-seed
swift scripts/verify-beta1.swift /path/to/beta1/libexec/bin .work/beta1-verification
```

명령의 종료 코드가 0이고 `report.json`의 `status`가 `verified`인지 확인합니다. 성공한 3세대 도구를 초기 컴파일러로 지정하여 공개 소스를 빌드합니다.

```sh
cd ../hmje
sh tools/build-from-release.sh ../hmje-swift-seed/.work/beta1-verification/beta1-gen3 "$PWD/build/native-bin"
HMJE_INSTALL_WARM=0 sh tools/install-native.sh "$PWD/build/install" "$PWD/build/native-bin"
HMJEC="$PWD/build/install/bin/hmjec" sh tools/test.sh
```

재현 검증과 공개 소스 검사는 범위가 다릅니다. 씨앗 검증은 네 컴파일러 도구의 재현성을 검사하며, 공개 소스 검사는 설치한 툴체인으로 예제와 스모크 검사를 실행합니다. IDE·다른 호스트 바이너리·앱 서명 인증서는 씨앗 검증 대상에 포함되지 않습니다.
