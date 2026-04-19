import {
  AfterViewInit,
  CUSTOM_ELEMENTS_SCHEMA,
  ChangeDetectionStrategy,
  Component,
  DestroyRef,
  ElementRef,
  effect,
  inject,
  input,
  output,
  signal,
  viewChild,
} from '@angular/core';
import { takeUntilDestroyed } from '@angular/core/rxjs-interop';
import * as L from 'leaflet';
import { ComandoDestino } from '../../core/models/comando.model';
import { Telemetria } from '../../core/models/telemetria.model';
import { formatarDistancia, haversineMetros } from '../../core/services/geo.util';
import { TelemetriaService } from '../../core/services/telemetria.service';

type CamadaTipo = 'dark' | 'street' | 'satellite';

const CAMADAS: Record<CamadaTipo, { url: string; attribution: string; maxZoom: number }> = {
  dark: {
    url: 'https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png',
    attribution: '©OpenStreetMap ©CartoDB',
    maxZoom: 19,
  },
  street: {
    url: 'https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
    attribution: '©OpenStreetMap',
    maxZoom: 19,
  },
  satellite: {
    url: 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
    attribution: 'Tiles © Esri',
    maxZoom: 18,
  },
};

@Component({
  selector: 'hd-map',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  schemas: [CUSTOM_ELEMENTS_SCHEMA],
  template: `
    <div #mapa class="mapa"></div>

    <div class="overlay mono">
      <div><iconify-icon icon="ph:target-bold"></iconify-icon> MODO: <span>TELECOMANDO</span></div>
      <div><iconify-icon icon="ph:arrow-down-bold"></iconify-icon> RX: <span>ws /telemetria</span></div>
      <div><iconify-icon icon="ph:arrow-up-bold"></iconify-icon> TX: <span>POST /comando</span></div>
    </div>

    @if (distanciaTexto()) {
      <div class="dist mono">
        <iconify-icon icon="ph:flag-pennant-fill"></iconify-icon>
        DISTÂNCIA AO DESTINO: <span>{{ distanciaTexto() }}</span>
      </div>
    }

    <div class="camadas" role="group" aria-label="Camadas do mapa">
      <div class="cam-titulo mono"><iconify-icon icon="ph:stack-bold"></iconify-icon> CAMADAS</div>
      @for (c of tiposCamada; track c) {
        <button
          type="button"
          class="btn-cam mono"
          [class.ativo]="camadaAtiva() === c"
          (click)="trocarCamada(c)">
          <iconify-icon [attr.icon]="iconeCamada(c)"></iconify-icon>
          {{ c.toUpperCase() }}
        </button>
      }
    </div>
  `,
  styleUrl: './map.component.scss',
})
export class MapComponent implements AfterViewInit {
  readonly telemetria = input<Telemetria | null>(null);
  readonly proa = input<number>(0);

  readonly destinoSelecionado = output<ComandoDestino>();

  private readonly mapaRef = viewChild.required<ElementRef<HTMLDivElement>>('mapa');
  private readonly telemetriaService = inject(TelemetriaService);
  private readonly destroyRef = inject(DestroyRef);

  private mapa?: L.Map;
  private tileLayer?: L.TileLayer;
  private marcadorBarco?: L.Marker;
  private marcadorDestino?: L.Marker;
  private linhaRota = L.polyline([], {
    color: '#ffc233',
    weight: 2,
    opacity: 0.85,
    dashArray: '10 6',
  });

  readonly tiposCamada: CamadaTipo[] = ['dark', 'street', 'satellite'];
  readonly camadaAtiva = signal<CamadaTipo>('dark');
  readonly distanciaTexto = signal<string>('');

  private posicaoAtual: [number, number] | null = null;
  private posicaoDestino: [number, number] | null = null;

  constructor() {
    effect(() => {
      const p = this.proa();
      if (!this.marcadorBarco) return;
      const el = this.marcadorBarco.getElement()?.querySelector<HTMLElement>('.boat-rot');
      if (el) el.style.transform = `rotate(${p}deg)`;
    });
  }

  ngAfterViewInit(): void {
    this.inicializarMapa();
    this.assinarPosicaoSuavizada();
  }

  private inicializarMapa(): void {
    this.mapa = L.map(this.mapaRef().nativeElement, { zoomControl: true }).setView(
      [-15.875, -48.085],
      15,
    );
    this.aplicarCamada('dark');
    this.linhaRota.addTo(this.mapa);

    this.mapa.on('click', (e: L.LeafletMouseEvent) => {
      const lat = +e.latlng.lat.toFixed(7);
      const lng = +e.latlng.lng.toFixed(7);
      this.definirDestino(lat, lng);
      this.destinoSelecionado.emit({ lat, lng });
    });
  }

  private assinarPosicaoSuavizada(): void {
    this.telemetriaService.posicaoSuavizada$
      .pipe(takeUntilDestroyed(this.destroyRef))
      .subscribe(([lat, lng]) => this.atualizarBarco(lat, lng));
  }

  private atualizarBarco(lat: number, lng: number): void {
    if (!this.mapa) return;
    this.posicaoAtual = [lat, lng];

    if (!this.marcadorBarco) {
      this.marcadorBarco = L.marker(this.posicaoAtual, {
        icon: L.divIcon({
          className: 'marker-barco',
          html: `
            <div class="boat-rot" style="transform: rotate(${this.proa()}deg)">
              <iconify-icon icon="ph:navigation-arrow-fill"></iconify-icon>
            </div>`,
          iconSize: [28, 28],
          iconAnchor: [14, 14],
        }),
      }).addTo(this.mapa);
    } else {
      this.marcadorBarco.setLatLng(this.posicaoAtual);
    }

    this.mapa.panTo(this.posicaoAtual, { animate: true, duration: 0.4 });
    this.redesenharRota();
    this.atualizarDistancia();
  }

  private definirDestino(lat: number, lng: number): void {
    if (!this.mapa) return;
    this.posicaoDestino = [lat, lng];

    if (!this.marcadorDestino) {
      this.marcadorDestino = L.marker(this.posicaoDestino, {
        icon: L.divIcon({
          className: 'marker-destino',
          html: '<iconify-icon icon="ph:map-pin-fill"></iconify-icon>',
          iconSize: [30, 30],
          iconAnchor: [15, 28],
        }),
      }).addTo(this.mapa);
    } else {
      this.marcadorDestino.setLatLng(this.posicaoDestino);
    }

    this.redesenharRota();
    this.atualizarDistancia();
  }

  private redesenharRota(): void {
    if (!this.posicaoAtual || !this.posicaoDestino) {
      this.linhaRota.setLatLngs([]);
      return;
    }
    this.linhaRota.setLatLngs([this.posicaoAtual, this.posicaoDestino]);
  }

  private atualizarDistancia(): void {
    if (!this.posicaoAtual || !this.posicaoDestino) {
      this.distanciaTexto.set('');
      return;
    }
    const m = haversineMetros(
      this.posicaoAtual[0],
      this.posicaoAtual[1],
      this.posicaoDestino[0],
      this.posicaoDestino[1],
    );
    this.distanciaTexto.set(formatarDistancia(m));
  }

  trocarCamada(tipo: CamadaTipo): void {
    this.camadaAtiva.set(tipo);
    this.aplicarCamada(tipo);
  }

  iconeCamada(tipo: CamadaTipo): string {
    return {
      dark: 'ph:moon-bold',
      street: 'ph:path-bold',
      satellite: 'ph:globe-bold',
    }[tipo];
  }

  private aplicarCamada(tipo: CamadaTipo): void {
    if (!this.mapa) return;
    if (this.tileLayer) this.mapa.removeLayer(this.tileLayer);
    const conf = CAMADAS[tipo];
    this.tileLayer = L.tileLayer(conf.url, {
      attribution: conf.attribution,
      maxZoom: conf.maxZoom,
    }).addTo(this.mapa);
  }
}
