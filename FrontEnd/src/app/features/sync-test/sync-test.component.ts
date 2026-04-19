import { ChangeDetectionStrategy, Component, inject, signal } from '@angular/core';
import { ComandoRapido } from '../../core/models/comando.model';
import { AlertsService } from '../../core/services/alerts.service';
import { ComandoService } from '../../core/services/comando.service';

interface LogSync {
  timestamp: string;
  acao: ComandoRapido | 'CONEXAO_OK' | 'RESP_OK' | 'RESP_ERR';
  detalhe: string;
  sucesso: boolean;
}

@Component({
  selector: 'hd-sync-test',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <div class="cabecalho mono">// teste de sincronização e2e</div>

    <div class="botoes">
      <button class="btn start" (click)="enviar('START')">
        <span class="mono">▶ START</span><small>Iniciar missão</small>
      </button>
      <button class="btn ack" (click)="enviar('ACK_ARRIVAL')">
        <span class="mono">✓ ACK</span><small>Confirmar chegada</small>
      </button>
      <button class="btn home" (click)="enviar('GO_HOME')">
        <span class="mono">⌂ HOME</span><small>Retornar à base</small>
      </button>
      <button class="btn stop" (click)="enviar('STOP')">
        <span class="mono">⏹ STOP</span><small>Parada de emergência</small>
      </button>
    </div>

    <div class="log-titulo mono">// log de confirmações</div>
    <div class="log">
      @for (l of logs(); track l.timestamp) {
        <div class="entrada mono" [class.ok]="l.sucesso" [class.err]="!l.sucesso">
          [{{ l.timestamp }}] {{ l.acao }} → {{ l.detalhe }}
        </div>
      } @empty {
        <div class="vazio mono">Sem eventos ainda.</div>
      }
    </div>
  `,
  styleUrl: './sync-test.component.scss',
})
export class SyncTestComponent {
  private readonly comandos = inject(ComandoService);
  private readonly alerts = inject(AlertsService);

  readonly logs = signal<LogSync[]>([]);

  enviar(acao: ComandoRapido): void {
    const stub = { lat: 0, lng: 0 };
    this.comandos.enviarDestino(stub).subscribe((resp) => {
      const ok = resp !== null;
      this.adicionarLog({
        timestamp: new Date().toLocaleTimeString('pt-BR', { hour12: false }),
        acao,
        detalhe: ok ? `publicado em ${resp?.topico}` : 'falha — broker indisponível',
        sucesso: ok,
      });
      if (ok) {
        this.alerts.emitir('info', 'Comando enviado', `${acao} transmitido via backend.`);
      } else {
        this.alerts.emitir('warning', 'Falha ao enviar', `${acao} não foi confirmado.`);
      }
    });
  }

  private adicionarLog(l: LogSync): void {
    this.logs.update((arr) => [l, ...arr].slice(0, 30));
  }
}
