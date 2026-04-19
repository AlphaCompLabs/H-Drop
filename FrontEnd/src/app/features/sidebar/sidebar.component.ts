import { ChangeDetectionStrategy, Component, input } from '@angular/core';
import { ComandoDestino } from '../../core/models/comando.model';
import { Telemetria } from '../../core/models/telemetria.model';
import { CompassComponent } from '../../shared/components/compass/compass.component';
import { TelemetryCardComponent } from '../../shared/components/telemetry-card/telemetry-card.component';

@Component({
  selector: 'hd-sidebar',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [CompassComponent, TelemetryCardComponent],
  template: `
    <section class="bloco">
      <div class="titulo mono">// proa magnética</div>
      <hd-compass [proa]="telemetria()?.proa ?? 0" />
    </section>

    <section class="bloco">
      <div class="titulo mono">// telemetria gps</div>
      <div class="grid">
        <hd-telemetry-card
          [label]="'Status do Sinal'"
          [valor]="statusLabel()"
          [tone]="telemetria()?.gps?.status === 'FIXADO' ? 'ok' : 'warning'"
          [destaque]="true" />
        <hd-telemetry-card
          [label]="'Latitude'"
          [valor]="telemetria()?.gps?.lat ?? '---'" />
        <hd-telemetry-card
          [label]="'Longitude'"
          [valor]="telemetria()?.gps?.lng ?? '---'" />
      </div>
    </section>

    <section class="bloco">
      <div class="titulo mono">// destino atual</div>
      @if (destino(); as d) {
        <div class="destino ativo mono">
          TRANSMITIDO ✓<br>
          LAT: {{ d.lat.toFixed(7) }}<br>
          LNG: {{ d.lng.toFixed(7) }}
        </div>
      } @else {
        <div class="destino mono">
          Nenhum destino enviado.<br>Clique no mapa para definir.
        </div>
      }
    </section>
  `,
  styleUrl: './sidebar.component.scss',
})
export class SidebarComponent {
  readonly telemetria = input<Telemetria | null>(null);
  readonly destino = input<ComandoDestino | null>(null);

  statusLabel(): string {
    return this.telemetria()?.gps.status === 'FIXADO' ? '✓ FIXADO' : '⚠ SEM SATÉLITE';
  }
}
