import { Injectable, OnDestroy, signal } from '@angular/core';
import { BehaviorSubject, Subject, Subscription, timer } from 'rxjs';
import { environment } from '../../../environments/environment';
import { EstadoConexao } from '../models/status.model';
import { Telemetria } from '../models/telemetria.model';

const RECONNECT_DELAY_MS = 3000;

@Injectable({ providedIn: 'root' })
export class TelemetriaService implements OnDestroy {
  private socket: WebSocket | null = null;
  private reconnectSub?: Subscription;

  readonly telemetria$ = new BehaviorSubject<Telemetria | null>(null);
  readonly estado = signal<EstadoConexao>('OFFLINE');
  readonly posicaoSuavizada$ = new Subject<[number, number]>();

  private bufferLat: number[] = [];
  private bufferLng: number[] = [];

  constructor() {
    this.conectar();
  }

  ngOnDestroy(): void {
    this.reconnectSub?.unsubscribe();
    this.socket?.close();
  }

  private conectar(): void {
    this.estado.set('CONECTANDO');
    try {
      this.socket = new WebSocket(environment.wsUrl);
    } catch {
      this.agendarReconexao();
      return;
    }

    this.socket.onopen = () => this.estado.set('ONLINE');

    this.socket.onmessage = (ev) => {
      try {
        const dados = JSON.parse(ev.data) as Telemetria;
        this.telemetria$.next(dados);
        if (dados.gps?.status === 'FIXADO' && dados.gps.lat !== null && dados.gps.lng !== null) {
          const suav = this.suavizar(dados.gps.lat, dados.gps.lng);
          this.posicaoSuavizada$.next(suav);
        }
      } catch {
        // payload inválido, ignora
      }
    };

    this.socket.onclose = () => {
      this.estado.set('OFFLINE');
      this.agendarReconexao();
    };

    this.socket.onerror = () => {
      this.socket?.close();
    };
  }

  private agendarReconexao(): void {
    this.reconnectSub?.unsubscribe();
    this.reconnectSub = timer(RECONNECT_DELAY_MS).subscribe(() => this.conectar());
  }

  private suavizar(lat: number, lng: number): [number, number] {
    this.bufferLat.push(lat);
    this.bufferLng.push(lng);
    const janela = environment.gpsSmoothingWindow;
    if (this.bufferLat.length > janela) {
      this.bufferLat.shift();
      this.bufferLng.shift();
    }
    const soma = (arr: number[]) => arr.reduce((a, b) => a + b, 0);
    const latMed = soma(this.bufferLat) / this.bufferLat.length;
    const lngMed = soma(this.bufferLng) / this.bufferLng.length;
    return [+latMed.toFixed(7), +lngMed.toFixed(7)];
  }

  limparBufferSuavizacao(): void {
    this.bufferLat = [];
    this.bufferLng = [];
  }
}
