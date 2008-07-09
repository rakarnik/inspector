#include "semodel.h"

SEModel::SEModel(const vector<string>& seqs, float** exprtab, const int numexpr, const vector<string>& names, const int nc, const int bf, const double map_cut, const double sim_cut):
seqset(seqs),
motif(seqset, nc),
select_motif(seqset, nc),
archive(seqset, map_cut, sim_cut),
ngenes(names.size()),
seqscores(ngenes),
seqranks(ngenes),
expscores(ngenes),
expranks(ngenes)
{
	npossible = 0;
	possible = new bool[ngenes];
	clear_all_possible();
	nameset = names;
	verbose = false;
	
	set_default_params();
  max_motifs = bf;
  map_cutoff = map_cut;
  sim_cutoff = sim_cut;
  freq_matrix = new int[motif.get_depth() * motif.ncols()];
  score_matrix = new double[motif.get_depth() * motif.ncols()];
	
	expr = exprtab;
	npoints = numexpr;
	mean = new float[npoints];
	stdev = new float[npoints];
	pcorr = new float*[ngenes];
	for(int i = 0; i < ngenes; i++) {
		pcorr[i] = new float[ngenes];
		for(int j = 0; j < ngenes; j++) {
			pcorr[i][j] = -2;
		}
	}
}

SEModel::~SEModel(){
  delete [] possible;
	delete [] freq_matrix;
  delete [] score_matrix;
	delete [] mean;
	delete [] stdev;
  for(int i=0;i<seqset.num_seqs();i++){
		delete [] pcorr[i];
  }
	delete [] pcorr;
}

void SEModel::set_default_params(){
  separams.expect = 10;
  separams.gcback = 0.38;
  separams.minpass = 50;
  separams.seed = -1;
  separams.psfact = 0.1;
  separams.weight = 0.8; 
  separams.npass = 1000000;
  separams.fragment = true;
  separams.flanking = 0;
  separams.undersample = 1;
  separams.oversample = 1;
	separams.minsize = 5;
	separams.mincorr = 0.4;
}

void SEModel::set_final_params(){
  separams.npseudo = separams.expect * separams.psfact;
  separams.backfreq[0]=separams.backfreq[5] = 0.25;
  separams.backfreq[1]=separams.backfreq[4] = (1 - separams.gcback)/2.0;
  separams.backfreq[2]=separams.backfreq[3] = separams.gcback/2.0;
  for(int i = 0; i < 6; i++){
    separams.pseudo[i] = separams.npseudo * separams.backfreq[i];
  }
  separams.maxlen = 3 * motif.width();
  separams.nruns = motif.positions_available() / separams.expect / motif.ncols() / separams.undersample * separams.oversample;
	separams.select = 5.0;
}

void SEModel::ace_initialize(){
  ran_int.set_seed(separams.seed);
  separams.seed = ran_int.seed();
  ran_int.set_range(0, RAND_MAX);
  ran_dbl.set_seed(ran_int.rnum());
  ran_dbl.set_range(0.0, 1.0);
}

void SEModel::add_possible(const int gene) {
	if (! possible[gene]) {
		possible[gene] = true;
		npossible++;
	}
}

void SEModel::remove_possible(const int gene) {
	if (possible[gene]) {
		possible[gene] = false;
		npossible--;
	}
}

void SEModel::clear_all_possible() {
	for(int g = 0; g < ngenes; g++) {
		possible[g] = false;
	}
	npossible = 0;
}

bool SEModel::is_possible(const int gene) const {
	return possible[gene];
}

int SEModel::possible_size() const {
	return npossible;
}

int SEModel::possible_positions() const {
	return motif.positions_available(possible);
}

void SEModel::clear_sites() {
	motif.clear_sites();
	select_motif.clear_sites();
}

int SEModel::size() const {
	return motif.seqs_with_sites();
}

int SEModel::motif_size() const {
	return motif.number();
}

bool SEModel::is_member(const int gene) const {
	return motif.seq_has_site(gene);
}

void SEModel::genes(int* genes) const {
	int count = 0;
	for (int g = 0; g < ngenes; g++) {
		if (is_member(g)) {
			genes[count] = g;
			count++;
		}
	}
	assert(count == size());
}

void SEModel::seed_random_site() {
	if(npossible < 1) return;
	
	motif.clear_sites();
	int chosen_possible, chosen_seq, chosen_posit;
  bool watson;
	
	chosen_seq = chosen_posit = -1;
	
	/* First choose a sequence */
	ran_int.set_range(0, possible_size() - 1);
	chosen_possible = ran_int.rnum();
	int g;
	for(g = 0; g < ngenes; g++) {
		if(is_possible(g) && chosen_possible == 0) break; 
		if(is_possible(g)) chosen_possible--;
	}
	chosen_seq = g;
	
	/* Now choose a site */
	int width = motif.width();
  for(int j = 0; j < 50; j++) {
		ran_int.set_range(0, seqset.len_seq(chosen_seq) - width - 1);
		double db = ran_dbl.rnum();//random (0,1)
		watson = (db > 0.5);
		chosen_posit = ran_int.rnum();
		if(watson && (chosen_posit > seqset.len_seq(chosen_seq) - width - 1)) continue;
		if((! watson) && (chosen_posit < width)) continue;
		if(motif.is_open_site(chosen_seq, chosen_posit)) {
      motif.add_site(chosen_seq, chosen_posit, watson);
      break;
    }
  }
}

void SEModel::calc_matrix() {
  motif.calc_score_matrix(score_matrix, separams.backfreq);
}

void SEModel::single_pass(const double minprob, bool greedy) {
	double ap = (separams.weight * separams.expect * 10
							+ (1 - separams.weight) * motif.number())
							/(2.0 * motif.positions_available(possible));
  calc_matrix();
	motif.remove_all_sites();
  select_motif.remove_all_sites();
  //will only update once per pass
	
  char **ss_seq;
  ss_seq = seqset.seq_ptr();
  double Lw, Lc, Pw, Pc, F;
	int matpos;
  int considered = 0;
  int gadd = -1,jadd = -1;
	int d = motif.get_depth();
	int nc = motif.ncols();
	int width = motif.width();
	int col, seq;
  for(int g = 0; g < seqset.num_seqs(); g++){
		if (! is_possible(g)) continue;
		for(int j = 0; j <= seqset.len_seq(g) - width; j++){
			Lw = 1.0;
			matpos = 0;
      for(int k = 0; k < nc; k++){
				col = motif.column(k);
				assert(j + col >= 0);
				assert(j + col <= seqset.len_seq(g));
				seq = ss_seq[g][j + col];
				Lw *= score_matrix[matpos + seq];
				matpos += d;
      }
      Lc = 1.0;
			matpos = d - 1;
      for(int k = 0; k < nc; k++){
				col = motif.column(k);
				assert(j + width - 1 - col >= 0);
				assert(j + width - 1 - col <= seqset.len_seq(g));
				seq = ss_seq[g][j + width - 1 - col];
				Lc *= score_matrix[matpos - seq];
				matpos += d;
      }
      Pw = Lw * ap/(1.0 - ap + Lw * ap);
      Pc = Lc * ap/(1.0 - ap + Lc * ap);
      F = Pw + Pc - Pw * Pc;//probability of either
			if(F > (minprob * 0.025/separams.select)) {
				select_motif.add_site(g, j, true);
			}
			//strand irrelevant for select_sites
			if(g == gadd && j < jadd + width) continue;
			if(F < minprob) continue;
			considered++;
			Pw = F * Pw / (Pw + Pc);
			Pc = F - Pw;
			if(greedy) {
				if(Pw > Pc) {
					assert(j >= 0);
					assert(j <= seqset.len_seq(g) - width);
					motif.add_site(g, j, true);
					gadd = g;
					jadd = j;
				} else {
					assert(j >= 0);
					assert(j <= seqset.len_seq(g) - width);
					motif.add_site(g, j, false);
					gadd = g;
					jadd = j;
				}
			} else {
				double r = ran_dbl.rnum();
				if (r > F) continue;
				else if (r < Pw) {
					assert(j >= 0);
					assert(j <= seqset.len_seq(g) - width);
					motif.add_site(g, j, true);
					gadd = g;
					jadd = j;
				} else {
					assert(j >= 0);
					assert(j <= seqset.len_seq(g) - width);
					motif.add_site(g, j, false);
					gadd = g;
					jadd = j;
				}
			}
    }
  }
  if(verbose)cerr<<"sampling "<<considered<<"/"<<select_motif.number()<<"\n";
}

void SEModel::single_pass_select(const double minprob){
	int i,j,k,n;
  double ap= (separams.weight * separams.expect + (1-separams.weight) * motif.number())
							/ (2.0 * motif.positions_available(possible));
  calc_matrix();
	motif.remove_all_sites();
  //will only update once per pass
	
  char **ss_seq = seqset.seq_ptr();
  double Lw, Lc, Pw, Pc, F;
  int matpos;
  int iadd = -1, jadd = -1;
	int selnum = select_motif.number();
	int width = select_motif.width();
	int d = select_motif.get_depth();
	int nc = select_motif.ncols();
  int col, seq;
	for(n = 0; n < selnum; n++){
    i = select_motif.chrom(n);
		if(! is_possible(i)) continue;
		j = select_motif.posit(n);
    if(i == iadd && j < jadd + width) continue;
    if(j < 0 || j > seqset.len_seq(i) - width) continue;
    //could be screwed up with column sampling
    Lw = 1.0;
		matpos = 0;
    for(k = 0; k < nc; k++){
			col = motif.column(k); 
      seq = ss_seq[i][j + col];
      Lw *= score_matrix[matpos + seq];
      matpos += d;
    }
    Lc = 1.0;
		matpos = d - 1;
    for(k = 0; k < nc; k++){
      col = motif.column(k); 
      seq = ss_seq[i][j + width - 1 - col];
      Lc *= score_matrix[matpos - seq];
      matpos += d;
    }
		Pw = Lw * ap / (1.0 - ap + Lw * ap);
    Pc = Lc * ap / (1.0 - ap + Lc * ap);
    F = (Pw + Pc - Pw * Pc);//probability of either
		if(F < minprob) continue;
		Pw = F * Pw/(Pw + Pc);
		Pc = F - Pw;
		double r = ran_dbl.rnum();
		if (r > F)
			continue;
    else if (r < Pw) {
      motif.add_site(i, j, true);
      add_possible(i);
			iadd = i;
			jadd = j;
    } else {
      motif.add_site(i, j, false);
			add_possible(i);
      iadd = i;
			jadd = j;
    }
  }
}

void SEModel::compute_seq_scores() {
	double ap = (separams.weight * separams.expect * 10
							+ (1 - separams.weight) * motif.number())
							/(2.0 * motif.positions_available(possible));
  calc_matrix();
	
  char **ss_seq;
  ss_seq = seqset.seq_ptr();
  double Lw, Lc, Pw, Pc, F, bestF;
	int matpos;
	seqranks.clear();
	int width = motif.width();
	int d = motif.get_depth();
	int nc = motif.ncols();
	int col, seq;
	for(int g = 0; g < seqset.num_seqs(); g++) {
		bestF = 0.0;
		for(int j = 0; j <= seqset.len_seq(g) - width; j++) {
			Lw = 1.0;
			matpos = 0;
			for(int k = 0; k < nc; k++) {
				col = motif.column(k);
				assert(j + col >= 0);
				assert(j + col <= seqset.len_seq(g));
				seq = ss_seq[g][j + col];
				Lw *= score_matrix[matpos + seq];
				matpos += d;
      }
      Lc = 1.0;
			matpos = d - 1;
      for(int k = 0; k < nc; k++){
				col = motif.column(k);
				assert(j + width - 1 - col >= 0);
				assert(j + width - 1 - col <= seqset.len_seq(g));
				int seq = ss_seq[g][j + width - 1 - col];
				Lc *= score_matrix[matpos - seq];
				matpos += d;
      }
      Pw = Lw * ap/(1.0 - ap + Lw * ap);
      Pc = Lc * ap/(1.0 - ap + Lc * ap);
      F = Pw + Pc - Pw * Pc;//probability of either
			if(F > bestF) bestF = F;
		}
		seqscores[g] = bestF;
		struct idscore ids;
		ids.id = g;
		ids.score = bestF;
		seqranks.push_back(ids);
  }
	
	sort(seqranks.begin(), seqranks.end(), isc);
}

void SEModel::compute_expr_scores() {
	calc_mean();
	expranks.clear();
	for(int g = 0; g < ngenes; g++) {
		expscores[g] = get_corr_with_mean(expr[g]);
		struct idscore ids;
		ids.id = g;
		ids.score = expscores[g];
		expranks.push_back(ids);
	}
	sort(expranks.begin(), expranks.end(), isc);
}  

void SEModel::column_sample(){
	int *freq = new int[motif.get_depth()];
	int width = motif.width();
	// Compute scores for current and surrounding columns
  int max_left, max_right;
  max_left = max_right = (motif.get_max_width() - width)/2;
  motif.columns_open(max_left, max_right);
	int cs_span = max_left + max_right + width;
  vector<struct idscore> wtx(cs_span);
	int x = max_left;
  //wtx[x + c] will refer to the weight of pos c in the usual numbering
	double wt, best_wt;
	for(int i = 0; i < cs_span; i++) {
		wtx[i].id = 1000;
		wtx[i].score = -DBL_MAX;
		if(motif.column_freq(i - x, freq)){
      wt = 0.0;
      for(int j = 0;j < motif.get_depth(); j++){
				wt += gammaln(freq[j] + separams.pseudo[j]);
				wt -= (double)freq[j] * log(separams.backfreq[j]);
      }
			wtx[i].id = i;
			wtx[i].score = wt;
			if(wt > best_wt) best_wt = wt;
    }
  }
	
	// Penalize outermost columns for length
  double scale = 0.0;
  if(best_wt > 100.0) scale = best_wt - 100.0; //keep exp from overflowing
	for(int i = 0; i < cs_span; i++){
		wtx[i].score -= scale;
		wtx[i].score = exp(wtx[i].score);
		int newwidth = width;
		if(i < x)
			newwidth += (x - i);
		else if(i > (x + width - 1))
			newwidth += (i - x - width + 1);
		wtx[i].score /= bico(newwidth - 2, motif.ncols() - 2);
	}
	
	// Sort columns by their score in wtx
	sort(wtx.begin(), wtx.end(), isc);
	
	// Keep the top <ncol> columns
	int ncols = motif.ncols();
	int nseen = 0;
	for(int i = 0; i < cs_span; ++i) {
		if(wtx[i].id - x > cs_span - 1)
			continue;
		if(nseen < ncols) {
			if(motif.has_col(wtx[i].id - x)) {        // column is ranked in top, and is already in motif
				nseen++;
			} else {
				motif.add_col(wtx[i].id - x);           // column is ranked in top, and is not in motif
				if(wtx[i].id - x < 0)                   // if column was to the left of the current columns, adjust the remaining columns
					for(int j = i + 1; j < cs_span; ++j)
						wtx[j].id -= wtx[i].id - x;
				nseen++;
			}
		} else {
			if(motif.has_col(wtx[i].id - x)) {        // column is not ranked in top, and is in motif
				if(wtx[i].id - x == 0)                  // we will remove the first column, adjust remaining columns
					for(int j = i + 1; j < cs_span; ++j)
						wtx[j].id -= motif.column(1);
				motif.remove_col(wtx[i].id - x);
			}
		}
	}
	
	if(motif.ncols() != ncols) {                 // number of columns should not change
		cerr << "ERROR: column samping started with " << ncols << ", ended with " << motif.ncols() << endl;
	}	
}

double SEModel::map_score() {
	int j,k;
  double ms = 0.0;
  double map_N = motif.positions_available(possible);  
  double w = separams.weight/(1.0-separams.weight);
  double map_alpha = (double) separams.expect * w;
  double map_beta = map_N * w - map_alpha;
  double map_success = (double)motif.number();
  ms += ( gammaln(map_success+map_alpha)+gammaln(map_N-map_success+map_beta) );
  ms -= ( gammaln(map_alpha)+gammaln(map_N + map_beta) );
	
  motif.calc_freq_matrix(freq_matrix);
  double sc[6]={0.0,0.0,0.0,0.0,0.0,0.0};
  int d=motif.get_depth();
  for(k=0;k!=d*motif.ncols();k+=d) {
    int x=freq_matrix[k]+freq_matrix[k+5];
    for(j=1;j<=4;j++){
      ms += gammaln((double)freq_matrix[k+j]+x*separams.backfreq[j]+separams.pseudo[j]);
      sc[j]+=freq_matrix[k+j]+x*separams.backfreq[j];
    }
  }
  ms-=motif.ncols()*gammaln((double)motif.number()+separams.npseudo);
  for (k=1;k<=4;k++) {
    ms -= sc[k]*log(separams.backfreq[k]);
  }
  /*This factor arises from a modification of the model of Liu, et al in which the background frequencies of DNA bases are taken to be constant for the organism under consideration*/
	double  vg;
  ms -= lnbico(motif.width()-2, motif.ncols()-2);
  for(vg=0.0,k=1;k<=4;k++) {
    vg += gammaln(separams.pseudo[k]);
  }
  vg-=gammaln((double)(separams.npseudo));
  ms-=((double)motif.ncols()*vg);
	return ms;
}

double SEModel::spec_score() {
	int x, s1, s2;
	x = s1 = s2 = 0;
	compute_seq_scores();
	compute_expr_scores();
	for(int g = 0; g < ngenes; g++) {
		if(seqscores[g] > motif.get_seq_cutoff()) s1++;
		if(expscores[g] > motif.get_expr_cutoff()) s2++;
		if(seqscores[g] > motif.get_seq_cutoff() && expscores[g] > motif.get_expr_cutoff()) x++;
	}
	
	double spec = prob_overlap(s1, s2, x, ngenes);
	if(spec > 0.99) return 0;
	else return -log10(spec);
}

string SEModel::consensus() const {
	map<char,char> nt;
  nt[0]=nt[5]='N';
  nt[1]='A';nt[2]='C';nt[3]='G';nt[4]='T';
	char** ss_seq=seqset.seq_ptr();
	
	int numsites = motif.number();
	if(numsites < 1) return "";
	
	int width = motif.width();
	vector<string> hits(numsites);
	for(int i = 0; i < numsites; i++){
		int c = motif.chrom(i);
    int p = motif.posit(i);
    bool s = motif.strand(i);
    for(int j = 0; j < width; j++){
      if(s) {
				if(p + j >= 0 && p+j < seqset.len_seq(c))
					 hits[i] += nt[ss_seq[c][p+j]];
				else hits[i] += ' ';
      } else {
				if(p + width - 1 - j >= 0 && p + width - 1 - j < seqset.len_seq(c))
					hits[i] += nt[motif.get_depth() - 1 - ss_seq[c][p + width - 1 - j]];
				else hits[i] += ' ';
      }
    }
  }
	
	string cons;
  int wide1 = hits[0].size();
  int num1 = hits.size();
  int num1a, num1c, num1g, num1t;
  for(int i = 0; i < wide1; i++){
    num1a = num1c = num1g = num1t = 0;
    for(int j = 0; j < num1; j++){
      if(hits[j][i] == 'a' || hits[j][i] == 'A') num1a += 1;
      if(hits[j][i] == 'c' || hits[j][i] == 'C') num1c += 1;
      if(hits[j][i] == 'g' || hits[j][i] == 'G') num1g += 1;
      if(hits[j][i] == 't' || hits[j][i] == 'T') num1t += 1;
    }
    if(num1a > num1*0.7) cons += 'A';
    else if(num1c > num1*0.7) cons += 'C';
    else if(num1g > num1*0.7) cons += 'G';
    else if(num1t > num1*0.7) cons += 'T';
    else if((num1a + num1t) > num1 * 0.85) cons +='W';
    else if((num1a + num1c) > num1 * 0.85) cons +='M';
    else if((num1a + num1g) > num1 * 0.85) cons +='R';
    else if((num1t + num1c) > num1 * 0.85) cons +='Y';
    else if((num1t + num1g) > num1 * 0.85) cons +='K';
    else if((num1c + num1g) > num1 * 0.85) cons +='S';
    else cons += '-';
  }
  return cons;
}

void SEModel::output_params(ostream &fout){
  fout<<" expect =      \t"<<separams.expect<<'\n';
  fout<<" gcback =      \t"<<separams.gcback<<'\n';
  fout<<" minpass =     \t"<<separams.minpass<<'\n';
  fout<<" seed =        \t"<<separams.seed<<'\n';
  fout<<" numcols =     \t"<<motif.ncols()<<'\n';
  fout<<" undersample = \t"<<separams.undersample<<'\n';
  fout<<" oversample = \t"<<separams.oversample<<'\n';
}

void SEModel::modify_params(int argc, char *argv[]){
  GetArg2(argc,argv,"-expect",separams.expect);
  GetArg2(argc,argv,"-gcback",separams.gcback);
  GetArg2(argc,argv,"-minpass",separams.minpass);
  GetArg2(argc,argv,"-seed",separams.seed);
  GetArg2(argc,argv,"-undersample",separams.undersample);
  GetArg2(argc,argv,"-oversample",separams.oversample);
}

void SEModel::calc_mean() {
	int i, j;
	for (i = 0; i < npoints; i++) {
		mean[i] = 0;
		for (j = 0; j < ngenes; j++)
			if(is_member(j))
				mean[i] += expr[j][i];
		mean[i] /= size();
	}
}

void SEModel::calc_stdev() {
	calc_mean();
	for(int i = 0; i < npoints; i++) {
		stdev[i] = 0;
		for(int j = 0; j < ngenes; j++)
			if(is_member(j))
				stdev[i] += (expr[j][i] - mean[i]) * (expr[j][i] - mean[i]);
		stdev[i] /= size() - 1;
		stdev[i] = sqrt(stdev[i]);
	} 
}

float SEModel::get_avg_pcorr() {
	float curr_avg_pcorr = 0.0;
	for(int g1 = 0; g1 < ngenes; g1++)
		for(int g2 = g1 + 1; g2 < ngenes; g2++)
			if(is_member(g1) && is_member(g2))
				curr_avg_pcorr += get_pcorr(g1, g2);
	curr_avg_pcorr = (2 * curr_avg_pcorr) / (size() * (size() - 1));
	return curr_avg_pcorr;
}

float SEModel::get_pcorr(const int g1, const int g2) {
	if(pcorr[g1][g2] == -2)
		pcorr[g1][g2] = pcorr[g2][g1] = corr(expr[g1], expr[g2], npoints);
	return pcorr[g1][g2];
}

float SEModel::get_corr_with_mean(const float* pattern) const {
	return corr(mean, pattern, npoints);
}

void SEModel::expand_search_around_mean(const double cutoff) {
	calc_mean();
	clear_all_possible();
	for(int g = 0; g < ngenes; g++)
		if(get_corr_with_mean(expr[g]) >= cutoff) add_possible(g);
}

void SEModel::expand_search_min_pcorr(const double cutoff) {
 	// First add all genes to list of candidates
 	list<int> candidates;
 	for(int g = 0; g < ngenes; g++)
 		candidates.push_back(g);
 	
 	// Now run through list of genes with sites, and only keep genes within minimum corr_cutoff
 	for(int g = 0; g < ngenes; g++) {
 		if(! is_member(g)) continue;
 		list<int> survivors;
 		for(list<int>::iterator iter = candidates.begin(); iter != candidates.end(); iter++)
 			if(get_pcorr(g, *iter) > cutoff) survivors.push_back(*iter);
 		candidates.assign(survivors.begin(), survivors.end());
 	}
 	
 	// Start with clean slate
 	clear_all_possible();
 	
 	// Add genes which already have sites to the search space
 	for(int g = 0; g < ngenes; g++)
 		if(is_member(g)) add_possible(g);
	for(list<int>::iterator iter = candidates.begin(); iter != candidates.end(); iter++)
		add_possible(*iter);
}

void SEModel::expand_search_avg_pcorr() {
	if(size() > 10) {
		motif.set_expr_cutoff(max(0.9 * get_avg_pcorr(), 0.4));
	}

	if(size() > 0) {
		float avg_pcorr;
		clear_all_possible();
		for(int g1 = 0; g1 < ngenes; g1++) {
			if(is_member(g1)) continue;
			avg_pcorr = 0;
			for(int g2 = 0; g2 < ngenes; g2++) {
				if(! is_member(g2)) continue;
				avg_pcorr += get_pcorr(g1, g2);
			}
			avg_pcorr /= size();
			if(avg_pcorr > motif.get_expr_cutoff()) add_possible(g1);
		}
		for(int g1 = 0; g1 < ngenes; g1++)
			if(is_member(g1)) add_possible(g1);
	}
}

void SEModel::search_for_motif(const int worker, const int iter) {
	motif.clear_sites();
	select_motif.clear_sites();
	motif.set_iter(iter);
	motif.set_expr_cutoff(0.85);
	motif.set_map(0);
	int i_worse = 0;
	int phase = 0;
	int oldphase = 0;
	
	Motif best_motif = motif;
	best_motif.set_spec(0.0);
	for(int g = 0; g < ngenes; g++)
		add_possible(g);
	seed_random_site();
	if(size() < 1) {
		cerr << "\t\t\tSeeding failed -- restarting..." << endl;
		return;
	}
	
	expand_search_around_mean(motif.get_expr_cutoff());
	while(possible_size() < separams.minsize && motif.get_expr_cutoff() > 0.4) {
		print_status(cerr, 0, oldphase);
		motif.set_expr_cutoff(motif.get_expr_cutoff() - 0.05);
		expand_search_around_mean(motif.get_expr_cutoff());
	}
	if(possible_size() < 2) {
		cerr << "\t\t\tBad search start -- no genes within " << separams.mincorr << endl;
		return;
	}
	print_status(cerr, 0, oldphase);
	compute_seq_scores();
	
	double ap = (double) separams.expect/(2.0 * motif.positions_available(possible));
	motif.set_seq_cutoff(ap * 5.0);
	
	for(int i = 1; i <= separams.npass; i++){
		if(oldphase < phase) {
			print_status(cerr, i, oldphase);
			oldphase = phase;
		}
		if(phase == 2) {
			if(size() < separams.minsize/2) {
				cerr << "\t\t\tReached phase " << phase << " with less than " << separams.minsize/2 << " sequences with motif. Restarting..." << endl;
				break;
			}
			double sc1 = motif.get_spec();
			for(int z = 0; motif.get_map() < sc1 && z < 5; z++){
				column_sample();
				single_pass(motif.get_seq_cutoff(), true);
				motif.set_spec(spec_score());
				print_status(cerr, i, phase);
			}
			if(motif.get_spec() < sc1) {
				motif = best_motif;
			}
			print_status(cerr, i, phase);
			if(size() < separams.minsize) {
				cerr << "\t\t\tCompleted phase " << phase << " with less than " << separams.minsize << " sequences with motif. Restarting..." << endl;
				break;
			}
			motif.orient();
			if(archive.check_motif(motif)) {
				char tmpfilename[30], motfilename[30];
				sprintf(tmpfilename, "%d.%d.mot.tmp", worker, iter);
				sprintf(motfilename, "%d.%d.mot", worker, iter);
				ofstream motout(tmpfilename);
				motif.write(motout);
				motout.close();
				rename(tmpfilename, motfilename);
				cerr << "\t\t\tCompleted phase " << phase << "! Restarting..." << endl;
			} else {
				cerr <<"\t\t\tToo similar! Restarting..." << endl;
			}
			break;
		}
		if(i_worse == 0) {
			single_pass(motif.get_seq_cutoff());
		} else {
			single_pass_select(motif.get_seq_cutoff());
		}
		if(motif_size() == 0) {
			if(best_motif.get_spec() == 0) {
				cerr << "\t\t\tNo sites and best score matched cutoff! Restarting..." << endl;
				break;
			}
			//if(best_motif.number()<4) break;
			motif = best_motif;
			select_motif = best_motif;
			phase++;
			i_worse = 0;
			continue;
		}
		if(i <= 3) continue;
		if(phase < 3 && size() > separams.minsize)
			column_sample();
		compute_seq_scores();
		compute_expr_scores();
		set_seq_cutoffs();
		set_expr_cutoffs();
		expand_search_around_mean(motif.get_expr_cutoff());
		if(motif.get_spec() - best_motif.get_spec() > 1e-3) {
			i_worse = 0;
			if(size() > separams.minsize * 2) {
				if(! archive.check_motif(motif)) {
					print_status(cerr, i, phase);
					cerr <<"\t\t\tToo similar! Restarting..." << endl;
					break;
				}
			}
			best_motif = motif;
		}
		else i_worse++;
		if(i_worse > separams.minpass * (phase + 1)){
			if(best_motif.get_spec() == 0) {
				print_status(cerr, i, phase);
				cerr << "\t\t\ti_worse is greater than cutoff and best score at cutoff! Restarting..." << endl;
				break;
			}
			if(best_motif.number() < 2) {
				print_status(cerr, i, phase);
				cerr << "\t\t\ti_worse is greater than cutoff and only 1 site! Restarting..." << endl;
				break;
			}
			print_status(cerr, i, phase);
			cerr << "\t\t\t\ti_worse threshold reached. Reloading best sites so far..." << endl;
			motif = best_motif;
			select_motif = best_motif;
			phase++;
			i_worse = 0;
		}
		
		if(size() > 5) {
			compute_seq_scores();
			compute_expr_scores();
			set_seq_cutoffs();
			set_expr_cutoffs();
			expand_search_around_mean(motif.get_expr_cutoff());
		}
	
		if(i % 50 == 0) {
			print_status(cerr, i, phase);
		}
	}
}

bool SEModel::consider_motif(const char* filename) {
	ifstream motin(filename);
	motif.clear_sites();
	motif.read(motin);
	bool ret = archive.consider_motif(motif);
	motin.close();
	return ret;
}

void SEModel::print_status(ostream& out, const int i, const int phase) {
	if(size() > 0) {
		out << "\t\t\t" << setw(5) << i;
		out << setw(3) << phase;
		int prec = cerr.precision(2);
		out << setw(5) << setprecision(2) << motif.get_expr_cutoff();
		out << setw(10) << setprecision(2) << motif.get_seq_cutoff();
		cerr.precision(prec);
		out << setw(5) << motif_size();
		out << setw(5) << size();
		out << setw(5) << possible_size();
		out << setw(40) << consensus();
		out << setw(10) << motif.get_spec();
		out << endl;
	} else {
		out << "\t\t\tNo sites!" << endl;
	}
}

void SEModel::full_output(ostream &fout){
  Motif* s;
	for(int j = 0; j < archive.nmots(); j++){
    s = archive.return_best(j);
    if(s->get_spec() > 1){
			fout << "Motif " << j + 1 << endl;
			s->write(fout);
		}
    else break;
  }
}

void SEModel::full_output(char *name){
  ofstream fout(name);
  full_output(fout);
}

void SEModel::set_seq_cutoffs() {
	int seqn, expn = 0, isect, next = 0;
	double best_po = 1;
	double best_sitecut = 0;
	double po, c;
	for(int i = 0; i < ngenes; i++)
		if(expscores[i] > motif.get_expr_cutoff()) expn++;
	for(c = seqranks[0].score; c > 0; c = seqranks[next].score) {
		seqn = isect = 0;
		for(int i = 0; i < ngenes; i++) {
			if(seqranks[i].score >= c) {
				seqn++;
				if(expscores[seqranks[i].id] >= motif.get_expr_cutoff())
					isect++;
			} else {
				next = i;
				break;
			}
		}
		po = prob_overlap(expn, seqn, isect, ngenes);
		// cerr << "\t\t\t\t\t\t" << c << ":\t" << isect << "/(" << seqn << "," << expn << ")/" << ngenes << "\t" << po << endl;
		if(po <= best_po) {
			best_po = po;
			best_sitecut = c;
		}
	}
	// cerr << "\t\t\t\t\tSetting sequence cutoff to " << best_sitecut << endl;
	motif.set_seq_cutoff(best_sitecut);
	if(best_po > 0.99)
		motif.set_spec(0);
	else
		motif.set_spec(-log10(best_po));
}

void SEModel::set_expr_cutoffs() {
	int seqn = 0, expn, isect;
	double best_po = 1;
	double best_exprcut = 0;
	double po, c;
	for(int i = 0; i < ngenes; i++)
		if(seqscores[i] >= motif.get_seq_cutoff()) seqn++;
	for(c = -1.0; c <= 1; c += 0.01) {
		expn = isect = 0;
		for(int i = 0; i < ngenes; i++) {
			if(expranks[i].score >= c) {
				expn++;
				if(seqscores[expranks[i].id] >= motif.get_seq_cutoff())
					isect++;
			} else {
				break;
			}
		}
		po = prob_overlap(seqn, expn, isect, ngenes);
		// cerr << "\t\t\t\t\t\t" << c << ":\t\t" << isect << "/(" << expn << "," << seqn << ")/" << ngenes << "\t\t" << po << endl; 
		if(po <= best_po) {
			best_po = po;
			best_exprcut = c;
		}
	}
	// cerr << "\t\t\t\t\tSetting expression cutoff to " << best_exprcut << endl;
	motif.set_expr_cutoff(best_exprcut);
	if(best_po > 0.99)
		motif.set_spec(0);
	else
		motif.set_spec(-log10(best_po));
}

void SEModel::print_possible(ostream& out) {
	for(int g = 0; g < ngenes; g++)
		if(is_possible(g)) out << nameset[g] << endl;
}

