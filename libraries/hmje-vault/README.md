# hmje-vault — 자격 증명 저장

운영체제별 자격 증명 저장과 사용자 인증을 요구하는 보호 저장 API를 제공합니다. 상태·오류를 명시적으로 반환하며 앱 데이터 디렉터리 조회 기능도 포함합니다.

## 사용

HMJE 1.0.0-beta.1 툴체인의 SDK에 포함됩니다. 소비 프로젝트의 모듈에 사용할 라이브러리를 의존성으로 선언하고, HMJE 소스에서 모듈을 가져옵니다. 내보내는 모듈은 HmjeVault입니다.

```hmjep
모듈 App의
    원본 경로는 "Sources/App"이고,
    의존 라이브러리는 HmjeVault이다.
```

```hmje
HmjeVault를 가져온다.
```

프로젝트·산출물의 전체 구성은 [프로젝트 명세](../../docs/project.md), 이 트리에서 SDK를 빌드·설치하는 절차는 [소스 빌드](../../docs/building.md)를 참고하십시오.

## 기능

| 구분 | 제공 내용 |
| --- | --- |
| 기본 저장 | 이름 공간·키로 바이트 및 문자열 읽기·쓰기·삭제 |
| 보호 저장 | 지원 확인·사용자 인증·비동기 작업 상태 |
| 앱 데이터 | 일반 파일용 앱 데이터 경로 조회 |

## 지원 범위와 사용 조건

Apple Keychain, Android Keystore 기반 암호화 저장, Linux Secret Service와 Windows 네이티브 저장 구현을 사용합니다. 보호 저장은 macOS·Windows에 구현되어 있으며 다른 대상에서는 미지원 결과를 반환합니다. Linux는 HMJE_VAULT_APP_ID와 Secret Service가 필요하고, 서비스가 없으면 일반 파일로 대체하지 않습니다. 앱 서명·권한·잠금 상태에 따라 저장 호출이 실패할 수 있습니다.

## 문서와 소스

- [모듈·플랫폼별 빌드 구성](Project.hmjep)
- [구현 소스](Sources/)
- [저장 형식과 플랫폼 계약](docs/storage-contract.md)

## 버전과 라이선스

Beta 1의 API는 안정화 중입니다. 고정된 원본 커밋은 [소스 출처](../../SOURCES.md), 공통 변경 사항은 [릴리스 노트](../../RELEASE_NOTES-1.0.0-beta.1.md)를 확인하십시오. 앱에 포함하는 배포에는 [런타임·라이브러리 라이선스](../../LICENSE-RUNTIME)가 적용됩니다.
