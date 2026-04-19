import { ChangeDetectionStrategy, Component, input } from '@angular/core';
import { StatusSistema } from '../../../core/models/status.model';

@Component({
  selector: 'hd-status-bar',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <div class="barra" role="status" aria-label="Status dos subsistemas">
      <div class="item" [class.ok]="status().websocket === 'ONLINE'" [class.warn]="status().websocket === 'CONECTANDO'" [class.crit]="status().websocket === 'OFFLINE'">
        <span class="dot"></span><span class="txt mono">WS · {{ status().websocket }}</span>
      </div>
      <div class="item" [class.ok]="status().mqtt === 'ONLINE'" [class.crit]="status().mqtt !== 'ONLINE'">
        <span class="dot"></span><span class="txt mono">MQTT · {{ status().mqtt }}</span>
      </div>
      <div class="item" [class.ok]="status().gps === 'FIXADO'" [class.warn]="status().gps !== 'FIXADO'">
        <span class="dot"></span><span class="txt mono">GNSS · {{ status().gps }}</span>
      </div>
      <div class="item" [class.ok]="toneBateria() === 'ok'" [class.warn]="toneBateria() === 'warn'" [class.crit]="toneBateria() === 'crit'">
        <span class="dot"></span>
        <span class="txt mono">BAT · {{ status().bateriaSoC === null ? '---' : (status().bateriaSoC + '%') }}</span>
      </div>
      <div class="item" [class.ok]="(status().sinal4G ?? 0) >= 60" [class.warn]="(status().sinal4G ?? 0) > 0 && (status().sinal4G ?? 0) < 60" [class.crit]="!status().sinal4G">
        <span class="dot"></span>
        <span class="txt mono">4G · {{ status().sinal4G === null ? '---' : (status().sinal4G + '%') }}</span>
      </div>
      <div class="item" [class.ok]="status().cargaPresente === true" [class.warn]="status().cargaPresente === false" [class.dim]="status().cargaPresente === null">
        <span class="dot"></span>
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

  labelCarga(): string {
    const c = this.status().cargaPresente;
    if (c === null) return '---';
    return c ? 'PRESENTE' : 'VAZIO';
  }
}
