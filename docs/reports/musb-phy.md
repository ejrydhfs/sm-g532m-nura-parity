# MUSB + MT6735 T-PHY parity report

## Files changed
- `drivers/usb/musb/mediatek.c`
- `drivers/phy/mediatek/phy-mtk-tphy.c`

Patch: `$M/patches/musb-phy.patch`

## Evidence
- Downstream Samsung/MTK: `drivers/misc/mediatek/usb20/mt6735/{usb20.c,usb20_otg_if.c,usb20_host.c,usb20_phy.c}`.
- ROM symbols: `musb_probe`, `mtk_otg_vbus_event`, `usb_phy_set_host_mode`, `usb_phy_poweron`, `usb_phy_recover`, `musb_session_restart`.
- ROM disassembly was used to confirm the T-PHY bus-force values and the `set -> delay -> clear` CKPDN/session sequences.

## Parity implemented
| Behavior | Downstream | Nura after patch |
|---|---|---|
| ID pin role detection | GPIO/EINT with 400 ms debounce | optional `id-gpios`, delayed-work debounce |
| Host VBUS | PMIC/charger boost | optional `vbus-supply` regulator driven by MUSB `set_vbus` |
| Host entry | force IDDIG/VBUSVALID, start session, MUSB host mode | `musb_set_host()` + T-PHY host force + VBUS enable |
| Device entry | clear session, force device, MUSB peripheral mode | `musb_set_peripheral()` + VBUS disable |
| VBUS error recovery | clear session, force PHY idle, 5 ms, restart session | `vbus_work` / `MTK_PHY_SUBMODE_SESSION_RESTART` |
| HS eye tuning | VRT/TERM/SQTH 2/2/1 host, 7/7/1 device | `mtk_u2_phy_eye_tuning()` |
| Charger detection | DP 100k mode, PUPD_BIST disabled | `mtk_u2_phy_charger_setup()` |
| Peripheral/gadget | preserved | preserved |

## Compile
```
make ARCH=arm64 LLVM=1 ... -j2 \
  drivers/usb/musb/mediatek.o \
  drivers/phy/mediatek/phy-mtk-tphy.o
```
Result: both objects compile cleanly (exit 0).

## DTS fragment
Optional, only needed to enable ID/VBUS handling on this board:
```dts
&usb2 {
	vbus-supply = <&vbus_otg>;
	id-gpios = <&pio 44 GPIO_ACTIVE_LOW>;
};
```
The exact ID GPIO and OTG boost regulator are board-dependent and were not
asserted without the ROM DTB; the driver works without them (cable role falls
back to the controller/role-switch default).

## Hardware-unverified
- ID GPIO number/polarity and 400 ms debounce on the real micro-B connector.
- VBUS boost regulator name and enable polarity.
- HS eye tuning and charger-detection interaction with the PMIC.
- Host-mode enumeration and over-current recovery.
