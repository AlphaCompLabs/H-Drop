import { ChangeDetectionStrategy, Component, computed, inject, signal } from '@angular/core';
import { toSignal } from '@angular/core/rxjs-interop';
import { ComandoDestino } from '../../core/models/comando.model';
import { StatusSistema } from '../../core/models/status.model';
import { AlertsService } from '../../core/services/alerts.service';
import { ComandoService } from '../../core/services/comando.service';
import { TelemetriaService } from '../../core/services/telemetria.service';
import { AlertOverlayComponent } from '../../shared/components/alert-overlay/alert-overlay.component';
import { StatusBarComponent } from '../../shared/components/status-bar/status-bar.component';
import { ThemeToggleComponent } from '../../shared/components/theme-toggle/theme-toggle.component';
import { MapComponent } from '../map/map.component';
import { SidebarComponent } from '../sidebar/sidebar.component';
import { SyncTestComponent } from '../sync-test/sync-test.component';

type AbaMobile = 'mapa' | 'telemetria' | 'sync';

@Component({
  selector: 'hd-mission-control',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [
    MapComponent,
    SidebarComponent,
    SyncTestComponent,
    StatusBarComponent,
    AlertOverlayComponent,
    ThemeToggleComponent,
  ],
  template: `
    <header class="header">
      <a class="logo" href="#" aria-label="H-DROP Mission Control">
        <img src="assets/images/logo-hdrop-dark.png"  class="logo-img dark"  alt="H-DROP" />
        <img src="assets/images/logo-hdrop-light.png" class="logo-img light" alt="H-DROP" />
        <span class="mono subtitulo">MISSION CONTROL</span>
      </a>
      <div class="acoes">
        <hd-theme-toggle />
        <div class="conexao mono"
             [class.on]="telemetria.estado() === 'ONLINE'"
             [class.mid]="telemetria.estado() === 'CONECTANDO'">
          {{ telemetria.estado() }}
        </div>
      </div>
    </header>

    <nav class="abas mono" aria-label="Seleção de aba">
      <button [class.ativa]="aba() === 'mapa'"       (click)="aba.set('mapa')">MAPA</button>
      <button [class.ativa]="aba() === 'telemetria'" (click)="aba.set('telemetria')">TELEMETRIA</button>
      <button [class.ativa]="aba() === 'sync'"       (click)="aba.set('sync')">SYNC TEST</button>
    </nav>

    <main class="principal" [attr.data-aba]="aba()">
      <aside class="painel-esq">
        <hd-sidebar [telemetria]="telemetriaAtual()" [destino]="destino()" />
      </aside>

      <section class="centro">
        <hd-map
          [telemetria]="telemetriaAtual()"
          [proa]="telemetriaAtual()?.proa ?? 0"
          (destinoSelecionado)="onDestino($event)" />
      </section>

      <aside class="painel-dir">
        <hd-sync-test />
      </aside>
    </main>

    <hd-status-bar [status]="statusAgregado()" />
    <hd-alert-overlay />
  `,
  styleUrl: './mission-control.component.scss',
})
export class MissionControlComponent {
  readonly telemetria = inject(TelemetriaService);
  private readonly comandos = inject(ComandoService);
  private readonly alerts = inject(AlertsService);

  readonly telemetriaAtual = toSignal(this.telemetria.telemetria$, { initialValue: null });
  readonly destino = signal<ComandoDestino | null>(null);
  readonly aba = signal<AbaMobile>('mapa');

  readonly statusAgregado = computed<StatusSistema>(() => {
    const t = this.telemetriaAtual();
    return {
      websocket: this.telemetria.estado(),
      mqtt: this.telemetria.estado() === 'ONLINE' ? 'ONLINE' : 'OFFLINE',
      gps: t?.gps?.status ?? 'SEM_SINAL',
      bateriaSoC: null,
      sinal4G: null,
      cargaPresente: null,
      distanciaObstaculo: null,
    };
  });

  onDestino(destino: ComandoDestino): void {
    this.destino.set(destino);
    this.comandos.enviarDestino(destino).subscribe((resp) => {
      if (resp) {
        this.alerts.emitir(
          'ok',
          'Destino transmitido',
          `Lat ${destino.lat.toFixed(5)} · Lng ${destino.lng.toFixed(5)}`,
        );
      } else {
        this.alerts.emitir(
          'critical',
          'Falha crítica de transmissão',
          'Não foi possível publicar o destino no broker MQTT.',
          true,
        );
      }
    });
  }
}
