# 저장 계약

## Android v1

모든 namespace/key는 NUL 없는 엄격한 UTF-8입니다. `app`은 ApplicationContext의 실제 package name입니다.

```
scope = UTF8("HMJE-VAULT-1") || 0x00 || UTF8(app) || 0x00 || UTF8(namespace)
AAD   = scope || 0x00 || UTF8(key)
alias = "hmje-vault.v1." || lowerhex(SHA256(scope))
path  = noBackupFilesDir/hmje-vault/v1/lowerhex(SHA256(scope))/lowerhex(SHA256(AAD)).bin
```

`HMJE-VAULT-1`은 12바이트입니다. 파일은 아래 순서로 저장하며 최대 65,572바이트입니다.

| 바이트 | 값 |
| --- | --- |
| 0–3 | ASCII `HMJV` |
| 4 | 버전 1 |
| 5 | IV 길이 12 |
| 6–7 | 예약 0 |
| 8–19 | 공급자가 생성한 12바이트 IV |
| 20 이후 | GCM 암호문과 마지막 16바이트 인증 태그 |

알 수 없는 버전, 잘린 파일, 크기 초과, 태그 실패, 암호문이 있는데 키가 없는 경우는 무결성 실패입니다. 키가 없다고 읽기 중 새 키를 만들지 않습니다. 앱/namespace/key AAD가 달라지면 같은 유효 암호문을 이식해도 인증되지 않습니다.

앱 전용 디렉터리는 0700, 파일/잠금 파일은 0600이며 디렉터리·파일 열기에 `O_NOFOLLOW`를 사용합니다. namespace별 `flock`으로 동일 앱 여러 스레드/프로세스의 키 생성과 쓰기를 직렬화합니다. 쓰기는 IV 기반 고유 임시 파일에 암호문을 쓰고 fsync/rename/fsync(directory)합니다. 성공 반환은 동기화 완료 후입니다. rename 이후 디렉터리 동기화 오류가 발생하면 새 값이 이미 보일 수 있으므로 저장 오류를 “확실히 원래 값”으로 해석하면 안 됩니다.

삭제는 정확히 해당 암호문 파일만 지웁니다. 같은 namespace의 다른 항목을 위해 AES 키는 남깁니다. 프로세스 강제 종료 때 남은 `.pending-` 파일에는 암호문만 있으며 읽기 대상으로 선택하지 않습니다. 자동 pending 정리/키 회전/롤백 방지는 현재 없습니다.

## iOS

```
service = "net.semtle.hmje-vault.v1." + actualBundleID + "." + lowerhex(UTF8(namespace))
account = lowerhex(UTF8(key))
class   = kSecClassGenericPassword
```

기본 앱 Keychain access group을 사용하며 API로 다른 access group을 지정할 수 없습니다. 실제 접근 통제는 앱 서명/Keychain 그룹 권한이 담당합니다. bundle ID 문자열 접두사만을 OS 보안 경계로 주장하지 않습니다. 항목은 `AfterFirstUnlockThisDeviceOnly`, 비동기화 상태입니다. 읽기/삭제는 정확한 service와 account를 지정합니다. 쓰기는 update 후 missing이면 add, 중간 다른 프로세스 insert로 duplicate이면 update를 한 번 더 시도합니다.

Keychain 내부 암호문은 앱이 직접 열거나 변조하지 않습니다. iOS의 암호문 변조 보호는 OS의 Keychain 계약이며 이 저장소의 파일 변조 시험으로 검증했다고 주장하지 않습니다. Simulator에서 확인하는 API·서명·속성·프로세스 재시작 동작은 실물 기기의 잠금·재부팅·Secure Enclave 보호 검증을 대신하지 않습니다.

## Linux

`HMJE_VAULT_APP_ID`(ASCII 영문·숫자·점·밑줄·하이픈)를 앱 식별자로 사용합니다. 기본 Secret Service collection의 `xdg:schema=net.semtle.hmje-vault.v1`, `hmje-app`, namespace/key의 UTF-8 바이트를 16진수로 인코딩한 속성으로 항목을 찾습니다. 값은 `application/octet-stream` SecretValue이며 길이 0과 누락을 구분합니다. Secret Service가 없는 경우 파일로 대체하지 않습니다. `앱데이터경로()`는 별도 일반 파일 디렉터리입니다.
