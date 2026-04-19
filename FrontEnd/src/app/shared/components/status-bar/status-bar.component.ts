import { CUSTOM_ELEMENTS_SCHEMA, ChangeDetectionStrategy, Component, input } from '@angular/core';
import { StatusSistema } from '../../../core/models/status.model';

@Component({
  selector: 'hd-status-bar',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  schemas: [CUSTOM_ELEMENTS_SCHEMA],
  template: `
    <div class="barra" role="status" aria-label="Status dos subsistemas">
      <div class="item"
        [class.ok]="status().websocket === 'ONLINE'"
        [class.warn]="status().websocket === 'CONECTANDO'"
        [class.crit]="status().websocket === 'OFFLINE'">
        <iconify-icon [attr.icon]="iconeWs()" aria-hidden="true"></iconify-icon>
        <span class="txt mono">WS · {{ status().websocket }}</span>
      </div>

      <div class="item"
        [class.ok]="status().mqtt === 'ONLINE'"
        [class.crit]="status().mqtt !== 'ONLINE'">
        <iconify-icon icon="ph:cloud-bold" aria-hidden="true"></iconify-icon>
        <span class="txt mono">MQTT · {{ status().mqtt }}</span>
      </div>

      <div class="item"
        [class.ok]="status().gps === 'FIXADO'"
        [class.warn]="status().gps !== 'FIXADO'">
        <iconify-icon icon="ph:gps-bold" aria-hidden="true"></iconify-icon>
        <span class="txt mono">GNSS · {{ status().gps }}</span>
      </div>

      <div class="item"
        [class.ok]="toneBateria() === 'ok'"
        [class.warn]="toneBateria() === 'warn'"
        [class.crit]="toneBateria() === 'crit'">
        <iconify-icon [attr.icon]="iconeBateria()" aria-hidden="true"></iconify-icon>
        <span class="txt mono">BAT · {{ status().bateriaSoC === null ? '---' : (status().bateriaSoC + '%') }}</span>
      </div>

      <div class="item"
        [class.ok]="(status().sinal4G ?? 0) >= 60"
        [class.warn]="(status().sinal4G ?? 0) > 0 && (status().sinal4G ?? 0) < 60"
        [class.crit]="!status().sinal4G">
        <iconify-icon [attr.icon]="iconeSinal4G()" aria-hidden="true"></iconify-icon>
        <span class="txt mono">4G · {{ status().sinal4G === null ? '---' : (status().sinal4G + '%') }}</span>
      </div>

      <div class="item"
        [class.ok]="status().cargaPresente === true"
        [class.warn]="status().cargaPresente === false"
        [class.dim]="status().cargaPresente === null">
        <iconify-icon icon="ph:package-bold" aria-hidden="true"></iconify-icon>
        <span class="txt mono">CARGA · {{ labelCarga() }}</span>
      </div>
    </div>
  `,
  styleUrl: './status-bar.component.scss',
})
export class StatusBarComponent {
  readonly status = input.required<StatusSistema>();

  toneBateria(): 'ok' | 'warn' | 'crit' | 'dim' {
    const soc = this.status().bateriaSoC;
    if (soc === null) return 'dim';
    if (soc <= 15) return 'crit';
    if (soc <= 30) return 'warn';
    return 'ok';
  }

  iconeBateria(): string {
    const soc = this.status().bateriaSoC;
    if (soc === null) return 'ph:battery-empty-bold';
    if (soc <= 15) return 'ph:battery-warning-bold';
    if (soc <= 30) return 'ph:battery-low-bold';
    if (soc <= 60) return 'ph:battery-medium-bold';
    if (soc <= 85) return 'ph:battery-high-bold';
    return 'ph:battery-full-bold';
  }

  iconeSinal4G(): string {
    const s = this.status().sinal4G ?? 0;
    if (s === 0) return 'ph:cell-signal-slash-bold';
    if (s < 40) return 'ph:cell-signal-low-bold';
    if (s < 70) return 'ph:cell-signal-medium-bold';
    return 'ph:cell-signal-high-bold';
  }

  iconeWs(): string {
    const e = this.status().websocket;
    if (e === 'ONLINE') return 'ph:broadcast-bold';
    if (e === 'CONECTANDO') return 'ph:circle-notch-bold';
    return 'ph:plugs-bold';
  }

  labelCarga(): string {
    const c = this.status().cargaPresente;
    if (c === null) return '---';
    return c ? 'PRESENTE' : 'VAZIO';
  }
}
